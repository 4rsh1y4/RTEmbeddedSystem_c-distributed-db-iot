/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: master/main.cpp
 * Description: API Gateway (Master) node with Async Routing and Memcached Layer
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <chrono> // برای محاسبه زمان پاسخ‌دهی
#include <libmemcached/memcached.h> // کتابخانه Memcached

using namespace std;

map<string, string> config;
memcached_st *memc = nullptr; // متغیر سراسری کش

// ==========================================
// توابع کمکی برای Memcached
// ==========================================

// خواندن از کش
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

// نوشتن در کش (TTL = 60 ثانیه)
void set_to_cache(const string& key, const string& value) {
    if (!memc) return;
    memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), 60, 0);
}

// تابع کمکی برای اضافه کردن زمان پاسخ‌دهی به انتهای رشته JSON
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
                 "FROM sensors s "
                 "JOIN sensor_readings r ON s.sensor_id = r.sensor_id "
                 "WHERE s.sensor_id = ? "
                 "ORDER BY r.recorded_at DESC LIMIT 1";
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
// مدیریت ارتباطات و رویدادهای سرور
// ==========================================

// ساختاری برای نگهداری وضعیت درخواست‌هایی که به Slaveها فوروارد می‌شوند
struct RequestState {
    struct mg_connection *client_c; // اتصال کلاینت اصلی
    string sensor_id;               // آیدی سنسور مورد جستجو
    int current_slave;              // شماره Slave در حال بررسی
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time; // زمان شروع درخواست
};

// تابع مدیریت رویدادهای مربوط به ارتباط Master با Slaveها
static void slave_client_handler(struct mg_connection *nc, int ev, void *ev_data) {
    RequestState *state = (RequestState *) nc->fn_data;
    if (state == nullptr) return;

    if (ev == MG_EV_CONNECT) {
        cout << "[Master->Slave" << state->current_slave << "] Connected. Sending HTTP request..." << endl;
        mg_printf(nc, "GET /api/sensor?id=%s HTTP/1.0\r\nHost: localhost\r\n\r\n", state->sensor_id.c_str());
    } 
    else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        string response(hm->body.buf, hm->body.len);
        
        if (response.find("\"error\"") == string::npos && !response.empty()) {
            cout << "[Master] Data found in Slave " << state->current_slave << "! Forwarding to client." << endl;
            
            // ذخیره داده‌های دریافتی از Slave در کش Master برای دسترسی سریع‌تر در آینده
            string cache_key = "sensor_" + state->sensor_id;
            set_to_cache(cache_key, response);

            // محاسبه زمان پاسخ‌دهی
            auto end_time = std::chrono::high_resolution_clock::now();
            double time_ms = std::chrono::duration<double, std::milli>(end_time - state->start_time).count();
            
            string final_json = inject_time_into_json(response, time_ms);
            mg_http_reply(state->client_c, 200, "Content-Type: application/json\r\n", "%s", final_json.c_str());
            
            delete state;
            nc->fn_data = nullptr;
        } else {
            if (state->current_slave == 1) {
                cout << "[Master] Not found in Slave 1. Checking Slave 2..." << endl;
                state->current_slave = 2;
                string target_url = config["SLAVE2_URL"] + "/api/sensor?id=" + state->sensor_id;
                
                struct mg_connection *c2 = mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
                if (!c2) {
                    mg_http_reply(state->client_c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Failed to connect to Slave 2\"}\n");
                    delete state;
                }
                nc->fn_data = nullptr;
            } else {
                cout << "[Master] Data not found in any Slave. Returning 404." << endl;
                mg_http_reply(state->client_c, 404, "Content-Type: application/json\r\n", "{\"error\": \"Sensor data not found in the network\"}\n");
                delete state;
                nc->fn_data = nullptr;
            }
        }
        nc->is_closing = 1;
    }
    else if (ev == MG_EV_ERROR) {
        if (state->current_slave == 1) {
            cout << "[Master] Slave 1 unreachable. Fallback to Slave 2..." << endl;
            state->current_slave = 2;
            string target_url = config["SLAVE2_URL"] + "/api/sensor?id=" + state->sensor_id;
            struct mg_connection *c2 = mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
            if (!c2) {
                mg_http_reply(state->client_c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Network routing failed\"}\n");
                delete state;
            }
            nc->fn_data = nullptr;
        } else {
            mg_http_reply(state->client_c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Network routing failed\"}\n");
            delete state;
            nc->fn_data = nullptr;
        }
    }
}

// Event Handler اصلی Master
static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        if (mg_match(hm->uri, mg_str("/api/sensor"), NULL)) {
            char id_var[32] = {0};
            mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));
            string sensor_id(id_var);
            
            cout << "\n[Master] New request received for sensor ID: " << sensor_id << endl;

            // ۱. شروع تایمر محاسبه سرعت
            auto start_time = std::chrono::high_resolution_clock::now();
            string cache_key = "sensor_" + sensor_id;

            // ۲. بررسی لایه کش (Memcached)
            string cached_data = get_from_cache(cache_key);

            if (!cached_data.empty()) {
                // ---- [Cache Hit] ----
                auto end_time = std::chrono::high_resolution_clock::now();
                double time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
                
                cout << "[Master] Cache HIT! Data retrieved from Memcached in " << time_ms << " ms." << endl;
                
                string final_json = inject_time_into_json(cached_data, time_ms);
                mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", final_json.c_str());
            } 
            else {
                // ---- [Cache Miss] ----
                cout << "[Master] Cache MISS! Checking local SQLite database..." << endl;
                string local_result = query_local_db(sensor_id);
                
                if (local_result.find("\"error\"") == string::npos) {
                    // داده در دیتابیس لوکال یافت شد
                    cout << "[Master] Data found locally. Storing in Cache..." << endl;
                    
                    // ذخیره در کش برای دفعات بعد
                    set_to_cache(cache_key, local_result);
                    
                    auto end_time = std::chrono::high_resolution_clock::now();
                    double time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
                    
                    string final_json = inject_time_into_json(local_result, time_ms);
                    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", final_json.c_str());
                } else {
                    // داده محلی نبود، فوروارد به اسلیوها
                    cout << "[Master] Data not local. Starting async routing to Slaves..." << endl;
                    
                    // پاس دادن زمان شروع به RequestState برای محاسبه‌ی نهایی در صورت یافتن در اسلیو
                    RequestState *state = new RequestState{c, sensor_id, 1, start_time};
                    string target_url = config["SLAVE1_URL"] + "/api/sensor?id=" + sensor_id;
                    
                    struct mg_connection *sc = mg_http_connect(c->mgr, target_url.c_str(), slave_client_handler, state);
                    if (!sc) {
                        cout << "[Master] Failed to initiate connection to Slave 1." << endl;
                        mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\": \"Failed to reach Slaves\"}\n");
                        delete state;
                    }
                }
            }
        } else {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <CONFIG_FILE_PATH>" << endl;
        cerr << "Example: " << argv[0] << " config.txt" << endl;
        return 1;
    }

    load_config(argv[1]);

    // اتصال به ممکشد
    memc = memcached_create(NULL);
    memcached_server_st *servers = NULL;
    memcached_return rc;
    
    servers = memcached_server_list_append(servers, "127.0.0.1", 11211, &rc);
    rc = memcached_server_push(memc, servers);
    
    if (rc == MEMCACHED_SUCCESS) {
        cout << "[Master] Connected to Memcached Cache successfully." << endl;
    } else {
        cerr << "[Master] Warning: Failed to connect to Memcached." << endl;
    }

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    
    string url_str = "http://" + config["MASTER_IP"] + ":" + config["MASTER_PORT"];
    
    cout << "[Master] Starting API Gateway on " << url_str << endl;
    mg_http_listen(&mgr, url_str.c_str(), ev_handler, NULL);

    while (true) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    if (memc) {
        memcached_free(memc);
    }
    return 0;
}