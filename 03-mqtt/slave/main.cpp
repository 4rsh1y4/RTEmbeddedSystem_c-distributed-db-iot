/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: slave/main.cpp
 * Description: HTTP Server for Slave Node handling Memcached Cache and local SQLite queries
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <cstdlib>
#include <ctime>
#include <libmemcached/memcached.h> // کتابخانه Memcached

using namespace std;

// متغیر سراسری مسیر دیتابیس
static string db_path = "";

// تنظیمات مشترک پروژه
map<string, string> config;

// متغیر سراسری کش
memcached_st *memc = nullptr;
time_t cache_ttl = 60;

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

// نوشتن در کش
void set_to_cache(const string& key, const string& value) {
    if (!memc) return;

    memcached_return_t rc = memcached_set(
        memc, key.c_str(), key.length(), value.c_str(), value.length(), cache_ttl, 0
    );

    if (rc != MEMCACHED_SUCCESS) {
        cerr << "[Slave ERROR] memcached_set failed for key '" << key
             << "': " << memcached_strerror(memc, rc) << endl;
    } else {
        cout << "[Slave] Successfully stored key '" << key << "' in cache." << endl;
    }
}

// ==========================================
// خواندن فایل تنظیمات
// ==========================================

void load_config(const string& filename) {
    ifstream file(filename);
    string line;

    if (!file.is_open()) {
        cerr << "[Slave] Cannot open config file: " << filename << endl;
        exit(1);
    }

    while (getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        size_t delimiter_pos = line.find('=');
        if (delimiter_pos != string::npos)
            config[line.substr(0, delimiter_pos)] = line.substr(delimiter_pos + 1);
    }
}

bool token_is_valid(struct mg_http_message *hm) {
    struct mg_str *token = mg_http_get_header(hm, "X-Master-Token");
    return token && mg_strcmp(*token, mg_str(config["MASTER_SLAVE_TOKEN"].c_str())) == 0;
}

// ==========================================
// ارتباط با دیتابیس لوکال SQLite
// ==========================================

string query_database(const string& sensor_type, const string& sensor_id) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *stmt = nullptr;
    string result_data = "{\"error\": \"Not found\"}\n";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return "{\"error\": \"Failed to open DB\"}\n";
    }

    string sql =
        "SELECT s.sensor_type, s.sensor_id, s.sensor_name, r.value, s.unit, r.recorded_at "
        "FROM sensors s "
        "JOIN sensor_readings r ON s.sensor_id = r.sensor_id "
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
// مدیریت رویدادهای سرور
// ==========================================

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *) ev_data;

    bool sensor_route = mg_match(hm->uri, mg_str("/api/sensor"), NULL);
    bool flush_route = mg_match(hm->uri, mg_str("/internal/cache/flush"), NULL);

    if (!sensor_route && !flush_route) {
        mg_http_reply(c, 404, "", "Not Found\n");
        return;
    }

    // فقط Master اجازه ارسال درخواست به Slave را دارد
    if (!token_is_valid(hm)) {
        cout << "[Slave] Rejected unauthorized direct request." << endl;

        mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                      "{\"error\": \"Forbidden: requests must come through Master\"}\n");
        return;
    }

    // خالی کردن کش برای Benchmark
    if (flush_route) {
        if (memc && memcached_flush(memc, 0) == MEMCACHED_SUCCESS)
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"status\": \"Slave cache flushed\"}\n");
        else
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                          "{\"error\": \"Cache flush failed\"}\n");

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

    cout << "\n[Slave] Received query for sensor type: "
         << sensor_type << ", ID: " << sensor_id << endl;

    string cache_key = "sensor_" + sensor_type + "_" + sensor_id;

    // ۱. بررسی کش Memcached
    string cached_data = get_from_cache(cache_key);

    if (!cached_data.empty()) {
        // ---- [Cache Hit] ----
        cout << "[Slave] Cache HIT! Data retrieved from Memcached." << endl;
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "%s", cached_data.c_str());
    } else {
        // ---- [Cache Miss] ----
        cout << "[Slave] Cache MISS! Querying local SQLite DB..." << endl;

        string db_result = query_database(sensor_type, sensor_id);

        if (db_result.find("\"error\"") == string::npos) {
            set_to_cache(cache_key, db_result);
            cout << "[Slave] Data found in SQLite and stored in cache." << endl;

            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "%s", db_result.c_str());
        } else {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                          "%s", db_result.c_str());
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0]
             << " <IP> <PORT> <DB_PATH> <CONFIG_FILE_PATH>" << endl;
        cerr << "Example: " << argv[0]
             << " 0.0.0.0 8001 slave1.db ../config.txt" << endl;
        return 1;
    }

    string ip = argv[1];
    string port = argv[2];
    db_path = argv[3];

    load_config(argv[4]);

    if (config["MASTER_SLAVE_TOKEN"].empty() ||
        config["MEMCACHED_HOST"].empty() ||
        config["MEMCACHED_PORT"].empty() ||
        config["CACHE_TTL"].empty()) {

        cerr << "[Slave] Missing token or Memcached configuration." << endl;
        return 1;
    }

    int memcached_port;

    try {
        memcached_port = stoi(config["MEMCACHED_PORT"]);
        cache_ttl = stoul(config["CACHE_TTL"]);
    } catch (...) {
        cerr << "[Slave] Invalid MEMCACHED_PORT or CACHE_TTL." << endl;
        return 1;
    }

    // راه‌اندازی و اتصال به Memcached
    memc = memcached_create(NULL);
    memcached_return_t rc;

    memcached_server_st *servers = memcached_server_list_append(
        NULL, config["MEMCACHED_HOST"].c_str(), memcached_port, &rc
    );

    if (!servers || memcached_server_push(memc, servers) != MEMCACHED_SUCCESS) {
        cerr << "[Slave] Failed to configure Memcached." << endl;

        if (servers) memcached_server_list_free(servers);
        if (memc) memcached_free(memc);

        return 1;
    }

    memcached_server_list_free(servers);

    cout << "[Slave] Connected to Memcached at "
         << config["MEMCACHED_HOST"] << ":" << memcached_port
         << " with TTL=" << cache_ttl << " seconds." << endl;

    string url_str = "http://" + ip + ":" + port;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    cout << "[Slave] Starting server on " << url_str << endl;
    cout << "[Slave] Connected to database: " << db_path << endl;

    if (!mg_http_listen(&mgr, url_str.c_str(), ev_handler, NULL)) {
        cerr << "[Slave] Failed to start server on " << url_str << endl;
        mg_mgr_free(&mgr);
        memcached_free(memc);
        return 1;
    }

    while (true) mg_mgr_poll(&mgr, 1000);

    mg_mgr_free(&mgr);
    if (memc) memcached_free(memc);

    return 0;
}