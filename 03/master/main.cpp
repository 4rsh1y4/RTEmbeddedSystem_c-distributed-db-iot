/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: master/main.cpp
 * Description: API Gateway (Master) node with MQTT & HTTP integration, Memcached and Async Routing
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <chrono>
#include <cstdlib>
#include <libmemcached/memcached.h>
#include <mosquitto.h>

using namespace std;

map<string, string> config;
memcached_st *memc = nullptr;
struct mosquitto *mosq = nullptr;
struct mg_mgr mgr;
time_t cache_ttl = 60;
int mqtt_qos = 1;

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

    memcached_return_t rc = memcached_set(
        memc, key.c_str(), key.length(), value.c_str(), value.length(), cache_ttl, 0
    );

    if (rc != MEMCACHED_SUCCESS)
        cerr << "[Master] Memcached set failed: " << memcached_strerror(memc, rc) << endl;
}

string inject_time_into_json(const string& json, double time_ms) {
    size_t pos = json.find_last_of('}');
    if (pos == string::npos) return json;

    char time_buf[32];
    snprintf(time_buf, sizeof(time_buf), "%.3f", time_ms);
    return json.substr(0, pos) + ", \"response_time_ms\": " + string(time_buf) + "}\n";
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
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        size_t delimiter_pos = line.find('=');
        if (delimiter_pos != string::npos)
            config[line.substr(0, delimiter_pos)] = line.substr(delimiter_pos + 1);
    }

    cout << "[Master] Configuration loaded successfully." << endl;
}

bool token_is_valid(struct mg_http_message *hm) {
    struct mg_str *token = mg_http_get_header(hm, "X-Master-Token");
    return token && mg_strcmp(*token, mg_str(config["MASTER_SLAVE_TOKEN"].c_str())) == 0;
}

string query_local_db(const string& sensor_type, const string& sensor_id) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;
    string result_data = "{\"error\": \"Not found\"}\n";

    if (sqlite3_open(config["MASTER_DB"].c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return "{\"error\": \"Failed to open Master DB\"}\n";
    }

    string sql =
        "SELECT s.sensor_type, s.sensor_id, s.sensor_name, r.value, s.unit, r.recorded_at "
        "FROM sensors s JOIN sensor_readings r ON s.sensor_id = r.sensor_id "
        "WHERE s.sensor_type = ? AND s.sensor_id = ? "
        "ORDER BY r.recorded_at DESC LIMIT 1";

    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sensor_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, sensor_id.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result_data = "{";
            result_data += "\"sensor_type\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) + "\", ";
            result_data += "\"sensor_id\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + "\", ";
            result_data += "\"sensor_name\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) + "\", ";
            result_data += "\"value\": " + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) + ", ";
            result_data += "\"unit\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) + "\", ";
            result_data += "\"recorded_at\": \"" + string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) + "\"";
            result_data += "}\n";
        }
    }

    if (stmt) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result_data;
}

// ==========================================
// مدیریت ارتباطات با Slaveها
// ==========================================

struct RequestState {
    struct mg_connection *client_c;
    bool is_mqtt;
    string sensor_type;
    string sensor_id;
    int current_slave;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
};

void send_final_response(RequestState *state, const string& json_res) {
    if (state->is_mqtt) {
        mosquitto_publish(mosq, NULL, config["MQTT_RESPONSE_TOPIC"].c_str(),
                          json_res.length(), json_res.c_str(), mqtt_qos, false);
    } else {
        mg_http_reply(state->client_c, 200, "Content-Type: application/json\r\n",
                      "%s", json_res.c_str());
    }
}

string build_slave_url(int slave, const string& type, const string& id) {
    string prefix = slave == 1 ? "SLAVE1" : "SLAVE2";
    return "http://" + config[prefix + "_IP"] + ":" + config[prefix + "_PORT"] +
           "/api/sensor?type=" + type + "&id=" + id;
}

static void slave_client_handler(struct mg_connection *nc, int ev, void *ev_data) {
    RequestState *state = (RequestState *) nc->fn_data;
    if (state == nullptr) return;

    if (ev == MG_EV_CONNECT) {
        mg_printf(nc,
                  "GET /api/sensor?type=%s&id=%s HTTP/1.0\r\n"
                  "Host: localhost\r\n"
                  "X-Master-Token: %s\r\n\r\n",
                  state->sensor_type.c_str(), state->sensor_id.c_str(),
                  config["MASTER_SLAVE_TOKEN"].c_str());
    }

    else if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        string response(hm->body.buf, hm->body.len);

        if (response.find("\"error\"") == string::npos && !response.empty()) {
            cout << "[Master] Data found in Slave " << state->current_slave << "!" << endl;

            auto end_time = std::chrono::high_resolution_clock::now();
            double time_ms = std::chrono::duration<double, std::milli>(
                end_time - state->start_time
            ).count();

            send_final_response(state, inject_time_into_json(response, time_ms));
            delete state;
            nc->fn_data = nullptr;
        }

        else if (state->current_slave == 1) {
            cout << "[Master] Not found in Slave 1. Checking Slave 2..." << endl;

            state->current_slave = 2;
            string target_url = build_slave_url(2, state->sensor_type, state->sensor_id);
            struct mg_connection *c2 =
                mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);

            if (!c2) {
                send_final_response(state, "{\"error\": \"Failed to connect to Slave 2\"}\n");
                delete state;
            }

            nc->fn_data = nullptr;
        }

        else {
            send_final_response(state, "{\"error\": \"Sensor data not found in the network\"}\n");
            delete state;
            nc->fn_data = nullptr;
        }

        nc->is_closing = 1;
    }

    else if (ev == MG_EV_ERROR) {
        if (state->current_slave == 1) {
            cout << "[Master] Slave 1 unreachable. Fallback to Slave 2..." << endl;

            state->current_slave = 2;
            string target_url = build_slave_url(2, state->sensor_type, state->sensor_id);
            struct mg_connection *c2 =
                mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);

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

// تابع مشترک پردازش درخواست‌های HTTP و MQTT
void process_sensor_request(const string& sensor_type, const string& sensor_id,
                            RequestState *state) {
    string cache_key = "sensor_" + sensor_type + "_" + sensor_id;
    string cached_data = get_from_cache(cache_key);

    if (!cached_data.empty()) {
        auto end_time = std::chrono::high_resolution_clock::now();
        double time_ms = std::chrono::duration<double, std::milli>(
            end_time - state->start_time
        ).count();

        cout << "[Master] Cache HIT! Time: " << time_ms << " ms." << endl;
        send_final_response(state, inject_time_into_json(cached_data, time_ms));
        delete state;
        return;
    }

    cout << "[Master] Cache MISS! Checking local SQLite database..." << endl;
    string local_result = query_local_db(sensor_type, sensor_id);

    if (local_result.find("\"error\"") == string::npos) {
        set_to_cache(cache_key, local_result);

        auto end_time = std::chrono::high_resolution_clock::now();
        double time_ms = std::chrono::duration<double, std::milli>(
            end_time - state->start_time
        ).count();

        send_final_response(state, inject_time_into_json(local_result, time_ms));
        delete state;
        return;
    }

    cout << "[Master] Data not local. Routing to Slaves..." << endl;

    string target_url = build_slave_url(1, sensor_type, sensor_id);
    struct mg_connection *sc =
        mg_http_connect(&mgr, target_url.c_str(), slave_client_handler, state);

    if (!sc) {
        send_final_response(state, "{\"error\": \"Failed to reach Slaves\"}\n");
        delete state;
    }
}

// ==========================================
// Event Handlerهای HTTP و MQTT
// ==========================================

// پیام MQTT با فرمت type|id
void mqtt_on_message(struct mosquitto *mosq, void *userdata,
                     const struct mosquitto_message *msg) {
    if (msg->payload == nullptr) return;

    string payload((char*) msg->payload, msg->payloadlen);
    size_t end_pos = payload.find_last_not_of(" \n\r\t");

    if (end_pos == string::npos) payload.clear();
    else payload.erase(end_pos + 1);

    size_t separator = payload.find('|');

    if (separator == string::npos || separator == 0 ||
        separator == payload.length() - 1) {

        string error = "{\"error\": \"MQTT payload must be in type|id format\"}\n";
        mosquitto_publish(mosq, NULL, config["MQTT_RESPONSE_TOPIC"].c_str(),
                          error.length(), error.c_str(), mqtt_qos, false);
        return;
    }

    string sensor_type = payload.substr(0, separator);
    string sensor_id = payload.substr(separator + 1);

    cout << "\n[Master MQTT] New request for type: "
         << sensor_type << ", ID: " << sensor_id << endl;

    RequestState *state = new RequestState{
        nullptr, true, sensor_type, sensor_id, 1,
        std::chrono::high_resolution_clock::now()
    };

    process_sensor_request(sensor_type, sensor_id, state);
}

// درخواست HTTP
static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    if (mg_match(hm->uri, mg_str("/internal/cache/flush"), NULL)) {
        if (!token_is_valid(hm))
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                          "{\"error\": \"Forbidden\"}\n");
        else if (memc && memcached_flush(memc, 0) == MEMCACHED_SUCCESS)
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"status\": \"Master cache flushed\"}\n");
        else
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                          "{\"error\": \"Cache flush failed\"}\n");

        return;
    }

    if (!mg_match(hm->uri, mg_str("/api/sensor"), NULL)) {
        mg_http_reply(c, 404, "", "Not Found\n");
        return;
    }

    char type_var[64] = {0};
    char id_var[64] = {0};

    int type_length = mg_http_get_var(&hm->query, "type", type_var, sizeof(type_var));
    int id_length = mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));

    if (type_length <= 0 || id_length <= 0) {
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                      "{\"error\": \"Both query parameters 'type' and 'id' are required\"}\n");
        return;
    }

    string sensor_type(type_var);
    string sensor_id(id_var);

    cout << "\n[Master HTTP] New request for type: "
         << sensor_type << ", ID: " << sensor_id << endl;

    RequestState *state = new RequestState{
        c, false, sensor_type, sensor_id, 1,
        std::chrono::high_resolution_clock::now()
    };

    process_sensor_request(sensor_type, sensor_id, state);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <CONFIG_FILE_PATH>" << endl;
        return 1;
    }

    load_config(argv[1]);

    const char *required_keys[] = {
        "MASTER_IP", "MASTER_PORT", "MASTER_DB",
        "SLAVE1_IP", "SLAVE1_PORT", "SLAVE2_IP", "SLAVE2_PORT",
        "MASTER_SLAVE_TOKEN", "MEMCACHED_HOST", "MEMCACHED_PORT", "CACHE_TTL",
        "MQTT_BROKER_HOST", "MQTT_BROKER_PORT",
        "MQTT_REQUEST_TOPIC", "MQTT_RESPONSE_TOPIC", "MQTT_QOS"
    };

    for (const char *key : required_keys) {
        if (config[key].empty()) {
            cerr << "[Master] Missing config value: " << key << endl;
            return 1;
        }
    }

    int memcached_port, mqtt_port;

    try {
        memcached_port = stoi(config["MEMCACHED_PORT"]);
        mqtt_port = stoi(config["MQTT_BROKER_PORT"]);
        cache_ttl = stoul(config["CACHE_TTL"]);
        mqtt_qos = stoi(config["MQTT_QOS"]);
    } catch (...) {
        cerr << "[Master] Invalid numeric configuration value." << endl;
        return 1;
    }

    // اتصال به Memcached
    memc = memcached_create(NULL);
    memcached_return_t memcached_rc;
    memcached_server_st *servers = memcached_server_list_append(
        NULL, config["MEMCACHED_HOST"].c_str(), memcached_port, &memcached_rc
    );

    if (!servers || memcached_server_push(memc, servers) != MEMCACHED_SUCCESS) {
        cerr << "[Master] Error connecting to Memcached." << endl;
        if (servers) memcached_server_list_free(servers);
        if (memc) memcached_free(memc);
        return 1;
    }

    memcached_server_list_free(servers);
    cout << "[Master] Connected to Memcached at "
         << config["MEMCACHED_HOST"] << ":" << memcached_port << endl;

    // اتصال به MQTT Broker
    mosquitto_lib_init();
    mosq = mosquitto_new("MasterGateway", true, NULL);

    if (!mosq) {
        cerr << "[Master] Failed to create MQTT client." << endl;
        memcached_free(memc);
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_message_callback_set(mosq, mqtt_on_message);

    if (mosquitto_connect(mosq, config["MQTT_BROKER_HOST"].c_str(),
                          mqtt_port, 60) != MOSQ_ERR_SUCCESS) {

        cerr << "[Master] Error connecting to MQTT Broker!" << endl;
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        memcached_free(memc);
        return 1;
    }

    mosquitto_subscribe(mosq, NULL, config["MQTT_REQUEST_TOPIC"].c_str(), mqtt_qos);

    cout << "[Master] Connected to MQTT Broker at "
         << config["MQTT_BROKER_HOST"] << ":" << mqtt_port
         << ". Subscribed to '" << config["MQTT_REQUEST_TOPIC"] << "'." << endl;

    // راه‌اندازی HTTP
    mg_mgr_init(&mgr);
    string url_str = "http://" + config["MASTER_IP"] + ":" + config["MASTER_PORT"];

    if (!mg_http_listen(&mgr, url_str.c_str(), http_ev_handler, NULL)) {
        cerr << "[Master] Failed to start HTTP Gateway on " << url_str << endl;
        mg_mgr_free(&mgr);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        memcached_free(memc);
        return 1;
    }

    cout << "[Master] HTTP Gateway listening on " << url_str << endl;

    while (true) {
        mg_mgr_poll(&mgr, 100);
        mosquitto_loop(mosq, 0, 1);
    }

    mg_mgr_free(&mgr);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    if (memc) memcached_free(memc);

    return 0;
}