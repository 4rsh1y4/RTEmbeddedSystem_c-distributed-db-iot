/*
 * Author: Arshia Noroozi
 * Student ID: 402170562
 * File: master/main.cpp
 * Description: API Gateway (Master) node with asynchronous routing to Slaves
 */

#include "mongoose.h"
#include <sqlite3.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace std;

map<string, string> config;

void load_config(const string& filename) {
    ifstream file(filename);
    string line;

    if (!file.is_open()) {
        cerr << "[Master] Error: Cannot open config file: " << filename << endl;
        exit(1);
    }

    while (getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const size_t delimiter_pos = line.find('=');
        if (delimiter_pos != string::npos) {
            config[line.substr(0, delimiter_pos)] = line.substr(delimiter_pos + 1);
        }
    }

    cout << "[Master] Configuration loaded successfully." << endl;
}

bool is_valid_identifier(const string& value) {
    if (value.empty() || value.size() > 64) return false;

    for (const unsigned char ch : value) {
        if (!isalnum(ch) && ch != '_' && ch != '-') return false;
    }
    return true;
}

string query_local_db(const string& sensor_type, const string& sensor_id) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    string result_data = "{\"error\": \"Not found\"}\n";
    const string db_path = config["MASTER_DB"];

    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        if (db != nullptr) sqlite3_close(db);
        return "{\"error\": \"Failed to open Master DB\"}\n";
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

struct RequestState {
    struct mg_connection* client_c;
    string sensor_type;
    string sensor_id;
    int current_slave;
};

string build_slave_url(int slave_number, const RequestState& state) {
    const string prefix = slave_number == 1 ? "SLAVE1" : "SLAVE2";
    const string base_url =
        "http://" + config[prefix + "_IP"] + ":" + config[prefix + "_PORT"];
    return base_url + "/api/sensor?type=" + state.sensor_type +
           "&id=" + state.sensor_id;
}

static void slave_client_handler(struct mg_connection* nc, int ev, void* ev_data) {
    RequestState* state = static_cast<RequestState*>(nc->fn_data);
    if (state == nullptr) return;

    if (ev == MG_EV_CONNECT) {
        cout << "[Master->Slave" << state->current_slave
             << "] Connected. Sending HTTP request..." << endl;
        mg_printf(nc,
                  "GET /api/sensor?type=%s&id=%s HTTP/1.0\r\n"
                  "Host: localhost\r\n"
                  "X-Master-Token: %s\r\n\r\n",
                  state->sensor_type.c_str(), state->sensor_id.c_str(),
                  config["MASTER_SLAVE_TOKEN"].c_str());
    } else if (ev == MG_EV_HTTP_MSG) {
        auto* hm = static_cast<struct mg_http_message*>(ev_data);
        const string response(hm->body.buf, hm->body.len);
        const int status = mg_http_status(hm);

        if (status == 200 && !response.empty()) {
            cout << "[Master] Data found in Slave " << state->current_slave
                 << ". Forwarding to client." << endl;
            mg_http_reply(state->client_c, 200,
                          "Content-Type: application/json\r\n", "%s",
                          response.c_str());
            delete state;
            nc->fn_data = nullptr;
        } else if (status == 404) {
            if (state->current_slave == 1) {
                cout << "[Master] Not found in Slave 1. Checking Slave 2..." << endl;
                state->current_slave = 2;
                const string target_url = build_slave_url(2, *state);

                struct mg_connection* c2 =
                    mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
                if (c2 == nullptr) {
                    mg_http_reply(state->client_c, 500,
                                  "Content-Type: application/json\r\n",
                                  "{\"error\": \"Failed to connect to Slave 2\"}\n");
                    delete state;
                }
                nc->fn_data = nullptr;
            } else {
                cout << "[Master] Data not found in any node. Returning 404." << endl;
                mg_http_reply(state->client_c, 404,
                              "Content-Type: application/json\r\n",
                              "{\"error\": \"Sensor type and ID not found in the network\"}\n");
                delete state;
                nc->fn_data = nullptr;
            }
        } else {
            cerr << "[Master] Slave " << state->current_slave
                 << " returned unexpected HTTP status " << status << endl;
            mg_http_reply(state->client_c, 502,
                          "Content-Type: application/json\r\n",
                          "{\"error\": \"Invalid response from Slave\"}\n");
            delete state;
            nc->fn_data = nullptr;
        }

        nc->is_closing = 1;
    } else if (ev == MG_EV_ERROR) {
        if (state->current_slave == 1) {
            cout << "[Master] Slave 1 unreachable. Falling back to Slave 2..." << endl;
            state->current_slave = 2;
            const string target_url = build_slave_url(2, *state);
            struct mg_connection* c2 =
                mg_http_connect(nc->mgr, target_url.c_str(), slave_client_handler, state);
            if (c2 == nullptr) {
                mg_http_reply(state->client_c, 500,
                              "Content-Type: application/json\r\n",
                              "{\"error\": \"Network routing failed\"}\n");
                delete state;
            }
            nc->fn_data = nullptr;
        } else {
            mg_http_reply(state->client_c, 500,
                          "Content-Type: application/json\r\n",
                          "{\"error\": \"Network routing failed\"}\n");
            delete state;
            nc->fn_data = nullptr;
        }
    }
}

static void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;

    auto* hm = static_cast<struct mg_http_message*>(ev_data);

    if (!mg_match(hm->uri, mg_str("/api/sensor"), nullptr)) {
        mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                      "{\"error\": \"Not found\"}\n");
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

    cout << "\n[Master] New request: type=" << sensor_type
         << ", id=" << sensor_id << endl;

    const string local_result = query_local_db(sensor_type, sensor_id);

    if (local_result.find("\"error\"") == string::npos) {
        cout << "[Master] Data found locally. Sending response." << endl;
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s",
                      local_result.c_str());
        return;
    }

    cout << "[Master] Data not local. Starting asynchronous routing..." << endl;
    auto* state = new RequestState{c, sensor_type, sensor_id, 1};
    const string target_url = build_slave_url(1, *state);

    struct mg_connection* sc =
        mg_http_connect(c->mgr, target_url.c_str(), slave_client_handler, state);
    if (sc == nullptr) {
        cerr << "[Master] Failed to initiate connection to Slave 1." << endl;
        mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                      "{\"error\": \"Failed to reach Slaves\"}\n");
        delete state;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <CONFIG_FILE_PATH>" << endl;
        cerr << "Example: " << argv[0] << " ../../config.txt" << endl;
        return 1;
    }

    load_config(argv[1]);

    const char* required_keys[] = {
        "MASTER_IP",   "MASTER_PORT", "MASTER_DB",
        "SLAVE1_IP",   "SLAVE1_PORT", "SLAVE1_DB",
        "SLAVE2_IP",   "SLAVE2_PORT", "SLAVE2_DB",
        "MASTER_SLAVE_TOKEN"};
    for (const char* key : required_keys) {
        if (config[key].empty()) {
            cerr << "[Master] Missing required configuration: " << key << endl;
            return 1;
        }
    }

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    const string url_str =
        "http://" + config["MASTER_IP"] + ":" + config["MASTER_PORT"];

    cout << "[Master] Starting API Gateway on " << url_str << endl;
    if (mg_http_listen(&mgr, url_str.c_str(), ev_handler, nullptr) == nullptr) {
        cerr << "[Master] Failed to listen on " << url_str << endl;
        mg_mgr_free(&mgr);
        return 1;
    }

    while (true) mg_mgr_poll(&mgr, 1000);

    mg_mgr_free(&mgr);
    return 0;
}
