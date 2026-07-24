/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: slave/main.cpp
 * Description: HTTP Server for Slave Node handling local SQLite sensor queries
 */

#include "mongoose.h"
#include <sqlite3.h>
#include <iostream>
#include <string>

using namespace std;

// متغیر سراسری برای مسیر دیتابیس که حالا به صورت داینامیک مقداردهی می‌شود
static string db_path = "";

// تابع اجرای کوئری روی دیتابیس SQLite و تبدیل نتیجه به فرمت JSON
string query_database(const string& sensor_id) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    string result_data = "{\"error\": \"Not found\"}";

    // استفاده از مسیر داینامیک دیتابیس
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        return "{\"error\": \"Failed to open DB\"}";
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
            result_data += "}";
        }
    }
    
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    
    return result_data;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        
        if (mg_match(hm->uri, mg_str("/api/sensor"), NULL)) {
            char id_var[32] = {0};
            mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));
            
            string sensor_id(id_var);
            cout << "[Slave] Received query for sensor ID: " << sensor_id << endl;
            
            string db_result = query_database(sensor_id);
            
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", db_result.c_str());
        } else {
            mg_http_reply(c, 404, "", "Not Found");
        }
    }
}

int main(int argc, char *argv[]) {
    // بررسی تعداد آرگومان‌های ورودی
    // برنامه انتظار دارد 3 آرگومان دریافت کند: IP، Port و مسیر فایل دیتابیس
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <IP> <PORT> <DB_PATH>" << endl;
        cerr << "Example: " << argv[0] << " 0.0.0.0 8001 slave1.db" << endl;
        return 1;
    }

    // دریافت مقادیر از خط فرمان
    string ip = argv[1];
    string port = argv[2];
    db_path = argv[3]; // مقداردهی مسیر دیتابیس

    // ساخت آدرس کامل سرور به صورت داینامیک
    string url_str = "http://" + ip + ":" + port;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    
    cout << "[Slave] Starting server on " << url_str << endl;
    cout << "[Slave] Connected to database: " << db_path << endl;
    
    // ارسال آدرس داینامیک به Mongoose
    mg_http_listen(&mgr, url_str.c_str(), ev_handler, NULL);

    while (true) {
        mg_mgr_poll(&mgr, 1000); 
    }

    mg_mgr_free(&mgr);
    return 0;
}