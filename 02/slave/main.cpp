/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: slave/main.cpp
 * Description: HTTP Server for Slave Node handling Memcached Cache and local SQLite queries
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <string>
#include <libmemcached/memcached.h> // کتابخانه Memcached

using namespace std;

// متغیر سراسری مسیر دیتابیس
static string db_path = "";

// متغیر سراسری کش
memcached_st *memc = nullptr; 

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
    memcached_return_t rc = memcached_set(memc, key.c_str(), key.length(), value.c_str(), value.length(), 60, 0);
    if (rc != MEMCACHED_SUCCESS) {
        cerr << "[Slave ERROR] memcached_set failed for key '" << key << "': " << memcached_strerror(memc, rc) << endl;
    } else {
        cout << "[Slave] Successfully stored key '" << key << "' in cache." << endl;
    }
}

// ==========================================
// ارتباط با دیتابیس لوکال SQLite
// ==========================================

string query_database(const string& sensor_id) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    string result_data = "{\"error\": \"Not found\"}\n";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return "{\"error\": \"Failed to open DB\"}\n";
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
// مدیریت رویدادهای سرور (Event Handler)
// ==========================================

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        if (mg_match(hm->uri, mg_str("/api/sensor"), NULL)) {
            char id_var[32] = {0};
            mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));
            
            string sensor_id(id_var);
            cout << "\n[Slave] Received query for sensor ID: " << sensor_id << endl;
            
            string cache_key = "sensor_" + sensor_id;
            
            // ۱. بررسی کش Memcached
            string cached_data = get_from_cache(cache_key);
            
            if (!cached_data.empty()) {
                // ---- [Cache Hit] ----
                cout << "[Slave] Cache HIT! Data retrieved from Memcached." << endl;
                mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", cached_data.c_str());
            } 
            else {
                // ---- [Cache Miss] ----
                cout << "[Slave] Cache MISS! Querying local SQLite DB..." << endl;
                string db_result = query_database(sensor_id);
                
                if (db_result.find("\"error\"") == string::npos) {
                    // ذخیره داده در کش برای درخواست‌های بعدی
                    set_to_cache(cache_key, db_result);
                    cout << "[Slave] Data found in SQLite and stored in cache." << endl;
                    
                    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", db_result.c_str());
                } else {
                    // در دیتابیس هم پیدا نشد
                    mg_http_reply(c, 404, "Content-Type: application/json\r\n", "%s", db_result.c_str());
                }
            }
        } else {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <IP> <PORT> <DB_PATH>" << endl;
        cerr << "Example: " << argv[0] << " 0.0.0.0 8001 slave1.db" << endl;
        return 1;
    }

    string ip = argv[1];
    string port = argv[2];
    db_path = argv[3];

    // --- راه‌اندازی و اتصال به Memcached ---
    memc = memcached_create(NULL);
    memcached_server_st *servers = NULL;
    memcached_return rc;
    
    servers = memcached_server_list_append(servers, "127.0.0.1", 11211, &rc);
    rc = memcached_server_push(memc, servers);
    
    if (rc == MEMCACHED_SUCCESS) {
        cout << "[Slave] Connected to Memcached Cache successfully." << endl;
    } else {
        cerr << "[Slave] Warning: Failed to connect to Memcached." << endl;
    }
    // ----------------------------------------

    string url_str = "http://" + ip + ":" + port;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    
    cout << "[Slave] Starting server on " << url_str << endl;
    cout << "[Slave] Connected to database: " << db_path << endl;
    
    mg_http_listen(&mgr, url_str.c_str(), ev_handler, NULL);

    while (true) {
        mg_mgr_poll(&mgr, 1000); 
    }

    // پاک‌سازی حافظه در هنگام خروج
    mg_mgr_free(&mgr);
    if (memc) {
        memcached_free(memc);
    }
    return 0;
}