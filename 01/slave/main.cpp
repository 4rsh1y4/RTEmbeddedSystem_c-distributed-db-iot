/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: slave/main.cpp
 * Description: HTTP server for a Slave node handling local SQLite sensor queries
 */

#include "mongoose.h"
#include <sqlite3.h>

#include <cctype>
#include <fstream>
#include <iostream>
#include <string>

using namespace std;

static string db_path;
static string master_token;


string load_master_token(const string& config_path) {
    ifstream file(config_path);
    if (!file.is_open()) {
        cerr << "[Slave] Cannot open shared config file: " << config_path << endl;
        return "";
    }

    string line;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t delimiter_pos = line.find('=');
        if (delimiter_pos == string::npos) continue;

        const string key = line.substr(0, delimiter_pos);
        if (key == "MASTER_SLAVE_TOKEN") {
            string value = line.substr(delimiter_pos + 1);
            while (!value.empty() && isspace(static_cast<unsigned char>(value.front()))) {
                value.erase(value.begin());
            }
            while (!value.empty() && isspace(static_cast<unsigned char>(value.back()))) {
                value.pop_back();
            }
            return value;
        }
    }
    return "";
}

bool is_valid_identifier(const string& value) {
    if (value.empty() || value.size() > 64) return false;

    for (const unsigned char ch : value) {
        if (!isalnum(ch) && ch != '_' && ch != '-') return false;
    }
    return true;
}

string query_database(const string& sensor_type, const string& sensor_id) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    string result_data = "{\"error\": \"Not found\"}\n";

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return "{\"error\": \"Failed to open DB\"}\n";
    }

    const char* sql =
        "SELECT s.sensor_type, s.sensor_id, s.sensor_name, r.value, "
        "       s.unit, r.recorded_at "
        "FROM sensors s "
        "JOIN sensor_readings r ON s.sensor_id = r.sensor_id "
        "WHERE s.sensor_type = ? AND s.sensor_id = ? "
        "ORDER BY r.recorded_at DESC LIMIT 1";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sensor_type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, sensor_id.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result_data = "{";
            result_data += "\"sensor_type\": \"" +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) + "\", ";
            result_data += "\"sensor_id\": \"" +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) + "\", ";
            result_data += "\"sensor_name\": \"" +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) + "\", ";
            result_data += "\"value\": " +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) + ", ";
            result_data += "\"unit\": \"" +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) + "\", ";
            result_data += "\"recorded_at\": \"" +
                           string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) + "\"";
            result_data += "}\n";
        }
    }

    if (stmt != nullptr) sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result_data;
}

static void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    auto* hm = static_cast<struct mg_http_message*>(ev_data);

    if (!mg_match(hm->uri, mg_str("/api/sensor"), nullptr)) {
        mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                      "{\"error\": \"Not found\"}\n");
        return;
    }

    // Slave endpoints are internal. Only Master knows and sends this token.
    struct mg_str* supplied_token = mg_http_get_header(hm, "X-Master-Token");
    if (supplied_token == nullptr ||
        mg_strcmp(*supplied_token, mg_str(master_token.c_str())) != 0) {
        cerr << "[Slave] Rejected unauthorized direct request." << endl;
        mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                      "{\"error\": \"Forbidden: requests must come through Master\"}\n");
        return;
    }

    char type_var[65] = {0};
    char id_var[65] = {0};
    const int type_length =
        mg_http_get_var(&hm->query, "type", type_var, sizeof(type_var));
    const int id_length =
        mg_http_get_var(&hm->query, "id", id_var, sizeof(id_var));

    if (type_length <= 0 || id_length <= 0) {
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                      "{\"error\": \"Both query parameters 'type' and 'id' are required\"}\n");
        return;
    }

    const string sensor_type(type_var);
    const string sensor_id(id_var);

    if (!is_valid_identifier(sensor_type) || !is_valid_identifier(sensor_id)) {
        mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                      "{\"error\": \"Invalid sensor type or ID format\"}\n");
        return;
    }

    cout << "[Slave] Received query: type=" << sensor_type
         << ", id=" << sensor_id << endl;

    const string db_result = query_database(sensor_type, sensor_id);
    if (db_result.find("\"error\": \"Not found\"") != string::npos) {
        mg_http_reply(c, 404, "Content-Type: application/json\r\n", "%s",
                      db_result.c_str());
    } else if (db_result.find("\"error\"") != string::npos) {
        mg_http_reply(c, 500, "Content-Type: application/json\r\n", "%s",
                      db_result.c_str());
    } else {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s",
                      db_result.c_str());
    }
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0] << " <IP> <PORT> <DB_PATH> <CONFIG_FILE_PATH>" << endl;
        cerr << "Example: " << argv[0] << " <IP> <PORT> <DB_PATH> ../../config.txt" << endl;
        return 1;
    }

    const string ip = argv[1];
    const string port = argv[2];
    db_path = argv[3];
    const string config_path = argv[4];
    master_token = load_master_token(config_path);

    if (master_token.empty()) {
        cerr << "[Slave] MASTER_SLAVE_TOKEN is missing from " << config_path << endl;
        return 1;
    }

    const string url_str = "http://" + ip + ":" + port;

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    cout << "[Slave] Starting server on " << url_str << endl;
    cout << "[Slave] Connected to database: " << db_path << endl;

    if (mg_http_listen(&mgr, url_str.c_str(), ev_handler, nullptr) == nullptr) {
        cerr << "[Slave] Failed to listen on " << url_str << endl;
        mg_mgr_free(&mgr);
        return 1;
    }

    while (true) mg_mgr_poll(&mgr, 1000);

    mg_mgr_free(&mgr);
    return 0;
}
