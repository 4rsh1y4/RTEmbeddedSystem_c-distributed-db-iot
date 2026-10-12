/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: master/main.cpp
 * Description: API Gateway (Master) node with MQTT & HTTP integration, Memcached, Async Routing and Logs API
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <chrono>
#include <libmemcached/memcached.h>
#include <mosquitto.h> // کتابخانه MQTT

using namespace std;

map<string, string> config;
memcached_st *memc = nullptr; 
struct mosquitto *mosq = nullptr; // متغیر سراسری کلاینت MQTT
struct mg_mgr mgr;                // متغیر سراسری Mongoose Manager (جدید)

// ==========================================
// توابع کمکی برای Memcached و زمان
// ==========================================

string get_from_cache(const string& key) {
    if (!memc) return "";
    size_t return_value_length;
    uint32_t flags;
    memcached_return_t rc;
    char* mem_val = memcached_get(memc, key.c_str(), key.length(), &return_value_length, &flags, &rc);
    if (rc == MEMCACHED_SUCCESS && mem_val) {
        string val(mem_val, return_value_length);
        free(mem_val);
        return val;
    }
    return "";
}

void set_to_cache(const string& key, const string& value) {
    if (!memc) return;
    memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), 60, 0);
}

string inject_time_into_json(const string& json, double time_ms) {
    size_t pos = json.find_last_of('}');
    if (pos != string::npos) {
        string result = json.substr(0, pos);
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "%.3f", time_ms);
        result += ", \"response_time_ms\": " + string(time_buf) + "}\n";
        return result;
    }
    return json;
}

// ==========================================
// لود تنظیمات و دیتابیس
// ==========================================

void load_config(const string& filename) {
    ifstream file(filename);
    string line;
    if (!file.is_open()) {
        cerr << "[Master] Error: Cannot open config file: " << filename << endl;
        exit(1);
    }
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t delimiter_pos = line.find('=');
        if (delimiter_pos != string::npos) {
            config[line.substr(0, delimiter_pos)] = line.substr(delimiter_pos + 1);
        }
    }
    cout << "[Master] Configuration loaded successfully." << endl;
}

string query_local_db(const string& sensor_id) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    string result_data = "{\"error\": \"Not found\"}\n";
    string db_path = config["MASTER_DB"];

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return "{\"error\": \"Failed to open Master DB\"}\n";
    }

    string sql = "SELECT s.sensor_name, r.value, s.unit, r.recorded_at "
                 "FROM sensors s JOIN sensor_readings r ON s.sensor_id = r.sensor_id "
                 "WHERE s.sensor_id = ? ORDER BY r.recorded_at DESC LIMIT 1";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sensor_id.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result_data = "{";
            result_data += "\"sensor_name\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) + "\", ";
            result_data += "\"value\": " + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + ", ";
            result_data += "\"unit\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) + "\", ";
            result_data += "\"recorded_at\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) + "\"";
            result_data += "}\n";
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result_data;
}

// ==========================================
// مدیریت ارتباطات با Slaveها
// ==========================================

struct RequestState {
    struct mg_connection *client_c; // اتصال HTTP کلاینت اصلی
    bool is_mqtt;                   // فلگ برای تشخیص اینکه درخواست از MQTT بوده یا HTTP
    string sensor_id;
    int current_slave;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
};

// ارسال پاسخ نهایی به کلاینت (HTTP یا MQTT)
void send_final_response(RequestState *state, const string& json_res) {
    if (state->is_mqtt) {
        // QoS 1 تعیین شده برای ارسال پاسخ
        mosquitto_publish(mosq, NULL, "sensor/response", json_res.length(), json_res.c_str(), 1, false);
    } else {
        mg_http_reply(state->client_c, 200, "Content-Type: application/json\r\n", "%s", json_res.c_str());
    }
}

static void slave_client_handler(struct mg_connection *nc, int ev, void *ev_data) {
    RequestState *state = (RequestState *) nc->fn_data;
    if (state == nullptr) return;

    if (ev == MG_EV_CONNECT) {
        mg_printf(nc, "GET /api/sensor?id=%s HTTP/1.0\r\nHost: localhost\r\n\r\n", state->sensor_id.c_str());
    } 
    else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        string response(hm->body.buf, hm->body.len);
        
        if (response.find("\"error\"") == string::npos && !response.empty()) {
            cout << "[Master] Data found in Slave " << state->current_slave << "!" << endl;
            auto end_time = std::chrono::high_resolution_clock::now();
            double time_ms = std::chrono::duration<double, std::milli>(end_time - state->start_time).count();
            
            string final_json = inject_time_into_json(response, time_ms);
            send_final_response(state, final_json);
            
            delete state;
            nc->fn_data = nullptr;
        } else {
            if (state->current_slave == 1) {
                state->current_slave = 2;
                string target_url = config["SLAVE2_URL"] + "/api/sensor?id=" + state->sensor_id;
                struct mg_connection *c2 = mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
                if (!c2) {
                    send_final_response(state, "{\"error\": \"Failed to connect to Slave 2\"}\n");
                    delete state;
                }
                nc->fn_data = nullptr;
            } else {
                send_final_response(state, "{\"error\": \"Sensor data not found in the network\"}\n");
                delete state;
                nc->fn_data = nullptr;
            }
        }
        nc->is_closing = 1;
    }
    else if (ev == MG_EV_ERROR) {
        if (state->current_slave == 1) {
            state->current_slave = 2;
            string target_url = config["SLAVE2_URL"] + "/api/sensor?id=" + state->sensor_id;
            struct mg_connection *c2 = mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
            if (!c2) {
                send_final_response(state, "{\"error\": \"Network routing failed\"}\n");
                delete state;
            }
            nc->fn_data = nullptr;
        } else {
            send_final_response(state, "{\"error\": \"Network routing failed\"}\n");
            delete state;
            nc->fn_data = nullptr;
        }
    }
}

// تابع پردازش منطق اصلی یافتن داده (مشترک بین HTTP و MQTT)
void process_sensor_request(string sensor_id, RequestState* state) {
    string cache_key = "sensor_" + sensor_id;
    string cached_data = get_from_cache(cache_key);

    if (!cached_data.empty()) {
        auto end_time = std::chrono::high_resolution_clock::now();
        double time_ms = std::chrono::duration<double, std::milli>(end_time - state->start_time).count();
        cout << "[Master] Cache HIT! Time: " << time_ms << " ms." << endl;
        string final_json = inject_time_into_json(cached_data, time_ms);
        send_final_response(state, final_json);
        delete state;
    } 
    else {
        cout << "[Master] Cache MISS! Checking local SQLite database..." << endl;
        string local_result = query_local_db(sensor_id);
        
        if (local_result.find("\"error\"") == string::npos) {
            set_to_cache(cache_key, local_result);
            auto end_time = std::chrono::high_resolution_clock::now();
            double time_ms = std::chrono::duration<double, std::milli>(end_time - state->start_time).count();
            string final_json = inject_time_into_json(local_result, time_ms);
            send_final_response(state, final_json);
            delete state;
        } else {
            cout << "[Master] Data not local. Routing to Slaves..." << endl;
            // استفاده مستقیم از mgr سراسری
            string target_url = config["SLAVE1_URL"] + "/api/sensor?id=" + sensor_id;
            struct mg_connection *sc = mg_http_connect(&mgr, target_url.c_str(), slave_client_handler, state);
            
            if (!sc) {
                send_final_response(state, "{\"error\": \"Failed to reach Slaves\"}\n");
                delete state;
            }
        }
    }
}

// ==========================================
// Event Handler های Mongoose (HTTP) و MQTT
// ==========================================

// دریافت درخواست از طریق MQTT
void mqtt_on_message(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    if (msg->payload == nullptr) return;
    string sensor_id((char*)msg->payload, msg->payloadlen);
    
    // پاکسازی فاصله‌های اضافی احتمالی در آیدی سنسور
    sensor_id.erase(sensor_id.find_last_not_of(" \n\r\t")+1);
    
    cout << "\n[Master MQTT] New request received on topic 'sensor/request' for ID: " << sensor_id << endl;
    
    RequestState *state = new RequestState{nullptr, true, sensor_id, 1, std::chrono::high_resolution_clock::now()};
    process_sensor_request(sensor_id, state);
}

// دریافت درخواست از طریق HTTP
static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        // ================= API قدیمی: خواندن آخرین وضعیت =================
        if (mg_match(hm->uri, mg_str("/api/sensor"), NULL)) {
            char id_var[32] = {0};
            mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));
            string sensor_id(id_var);
            
            cout << "\n[Master HTTP] New request received for ID: " << sensor_id << endl;
            RequestState *state = new RequestState{c, false, sensor_id, 1, std::chrono::high_resolution_clock::now()};
            process_sensor_request(sensor_id, state);
        } 
        
        // ================= API جدید: خواندن تاریخچه (بخش 5) =================
        else if (mg_match(hm->uri, mg_str("/api/logs"), NULL)) {
            char s_name[100] = "", s_id[50] = "", s_date[50] = "";
            
            mg_http_get_var(&hm->query, "sensor_name", s_name, sizeof(s_name));
            mg_http_get_var(&hm->query, "sensor_id", s_id, sizeof(s_id));
            mg_http_get_var(&hm->query, "date", s_date, sizeof(s_date));

            if (s_name[0] == '\0' || s_id[0] == '\0' || s_date[0] == '\0') {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\": \"Missing parameters. Required: sensor_name, sensor_id, date\"}\n");
                return;
            }

            sqlite3 *db;
            string db_path = config["MASTER_DB"];
            if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Failed to open Master DB\"}\n");
                return;
            }

            // حذف بررسی نام سنسور برای جلوگیری از تداخل با دیتای CSV
            string query = "SELECT strftime('%H:%M:%S', recorded_at) as time, value "
                           "FROM sensor_readings "
                           "WHERE sensor_id = '" + string(s_id) + "' "
                           "AND date(recorded_at) = '" + string(s_date) + "' "
                           "ORDER BY recorded_at ASC;";

            sqlite3_stmt *stmt;
            int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, NULL);
            
            string json_response = "{\n";
            json_response += "  \"sensor_name\": \"" + string(s_name) + "\",\n";
            json_response += "  \"sensor_id\": \"" + string(s_id) + "\",\n";
            json_response += "  \"date\": \"" + string(s_date) + "\",\n";
            json_response += "  \"values\": [\n";

            bool first = true;
            bool has_data = false;

            if (rc == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    has_data = true;
                    if (!first) json_response += ",\n";
                    
                    const unsigned char *time_val = sqlite3_column_text(stmt, 0);
                    const unsigned char *val_str = sqlite3_column_text(stmt, 1);
                    
                    json_response += "    {\n";
                    json_response += "      \"time\": \"" + string((const char*)time_val) + "\",\n";
                    json_response += "      \"value\": \"" + string((const char*)val_str) + "\"\n";
                    json_response += "    }";
                    
                    first = false;
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);

            json_response += "\n  ]\n}";

            if (!has_data) {
                // اضافه شدن نکست‌لاین برای خروجی تمیز
                mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"message\": \"No logs found for this sensor on the specified date.\"}\n");
            } else {
                // اضافه شدن نکست‌لاین برای خروجی تمیز
                mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s\n", json_response.c_str());
            }
        } 
        // ================= مسیرهای نامعتبر =================
        else {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <CONFIG_FILE_PATH>" << endl;
        return 1;
    }
    load_config(argv[1]);

    // اتصال به Memcached
    memc = memcached_create(NULL);
    memcached_server_st *servers = memcached_server_list_append(NULL, "127.0.0.1", 11211, NULL);
    if (memcached_server_push(memc, servers) == MEMCACHED_SUCCESS) {
        cout << "[Master] Connected to Memcached." << endl;
    }

    // مقداردهی و اتصال به MQTT Broker (Mosquitto)
    mosquitto_lib_init();
    mosq = mosquitto_new("MasterGateway", true, NULL);
    mosquitto_message_callback_set(mosq, mqtt_on_message);
    
    if (mosquitto_connect(mosq, "localhost", 1883, 60) == MOSQ_ERR_SUCCESS) {
        // عضویت در تاپیک درخواست با QoS 1
        mosquitto_subscribe(mosq, NULL, "sensor/request", 1);
        cout << "[Master] Connected to MQTT Broker. Subscribed to 'sensor/request'" << endl;
    } else {
        cerr << "[Master] Error connecting to MQTT Broker!" << endl;
    }

    mg_mgr_init(&mgr);
    string url_str = "http://" + config["MASTER_IP"] + ":" + config["MASTER_PORT"];
    mg_http_listen(&mgr, url_str.c_str(), http_ev_handler, NULL);
    cout << "[Master] HTTP Gateway listening on " << url_str << endl;

    // حلقه‌ی اصلی سیستم (مدیریت همزمان HTTP و MQTT)
    while (true) {
        mg_mgr_poll(&mgr, 100); 
        mosquitto_loop(mosq, 0, 1); // بررسی غیرمسدودکننده پیام‌های MQTT
    }

    mg_mgr_free(&mgr);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    if (memc) memcached_free(memc);
    return 0;
}