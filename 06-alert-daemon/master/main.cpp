#include <curl/curl.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

struct Sensor {
    string id;
    string type;
    string name;
};

map<string, string> config;

string trim(string value) {
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) return "";

    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool load_config(const string& path) {
    ifstream file(path);
    if (!file.is_open()) return false;

    string line;

    while (getline(file, line)) {
        line = trim(line);

        if (line.empty() || line[0] == '#') continue;

        size_t pos = line.find('=');

        if (pos != string::npos) {
            config[trim(line.substr(0, pos))] =
                trim(line.substr(pos + 1));
        }
    }

    return true;
}

vector<string> split_csv(const string& line) {
    vector<string> fields;
    string field;
    stringstream stream(line);

    while (getline(stream, field, ',')) {
        fields.push_back(trim(field));
    }

    return fields;
}

vector<Sensor> load_sensors(const string& data_dir) {
    vector<Sensor> sensors;
    set<string> seen;

    if (!fs::exists(data_dir)) {
        cerr << "[Alert] Data directory not found: "
             << data_dir << endl;
        return sensors;
    }

    for (const auto& entry : fs::directory_iterator(data_dir)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".csv") {
            continue;
        }

        ifstream file(entry.path());
        string line;

        if (!getline(file, line)) continue;

        vector<string> header = split_csv(line);

        int id_col = -1;
        int type_col = -1;
        int name_col = -1;

        for (size_t i = 0; i < header.size(); ++i) {
            if (header[i] == "sensor_id")
                id_col = static_cast<int>(i);
            else if (header[i] == "sensor_type")
                type_col = static_cast<int>(i);
            else if (header[i] == "sensor_name")
                name_col = static_cast<int>(i);
        }

        if (id_col < 0 || type_col < 0 || name_col < 0)
            continue;

        while (getline(file, line)) {
            vector<string> row = split_csv(line);

            int max_col = max(id_col, max(type_col, name_col));

            if (static_cast<int>(row.size()) <= max_col)
                continue;

            string key = row[id_col] + "|" + row[type_col];

            if (!row[id_col].empty() &&
                seen.insert(key).second) {
                sensors.push_back({
                    row[id_col],
                    row[type_col],
                    row[name_col]
                });
            }
        }
    }

    return sensors;
}

size_t write_callback(
    void *contents,
    size_t size,
    size_t nmemb,
    void *userp
) {
    size_t total = size * nmemb;

    static_cast<string *>(userp)->append(
        static_cast<char *>(contents),
        total
    );

    return total;
}

string url_encode(CURL *curl, const string& value) {
    char *encoded = curl_easy_escape(
        curl,
        value.c_str(),
        static_cast<int>(value.size())
    );

    if (!encoded) return value;

    string result(encoded);
    curl_free(encoded);

    return result;
}

string fetch_sensor(
    const Sensor& sensor,
    long timeout_seconds
) {
    CURL *curl = curl_easy_init();

    if (!curl) return "";

    string response;

    string url =
        "http://" + config["MASTER_IP"] +
        ":" + config["MASTER_PORT"] +
        "/api/sensor?type=" +
        url_encode(curl, sensor.type) +
        "&id=" +
        url_encode(curl, sensor.id);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        write_callback
    );
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &response
    );
    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        timeout_seconds
    );
    curl_easy_setopt(
        curl,
        CURLOPT_NOSIGNAL,
        1L
    );

    CURLcode result = curl_easy_perform(curl);

    long http_code = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &http_code
    );

    curl_easy_cleanup(curl);

    if (result != CURLE_OK ||
        http_code < 200 ||
        http_code >= 300) {
        return "";
    }

    return response;
}

string json_string(
    const string& json,
    const string& key
) {
    string token = "\"" + key + "\"";

    size_t pos = json.find(token);

    if (pos == string::npos) return "";

    pos = json.find(':', pos + token.size());

    if (pos == string::npos) return "";

    pos = json.find('"', pos + 1);

    if (pos == string::npos) return "";

    size_t end = json.find('"', pos + 1);

    if (end == string::npos) return "";

    return json.substr(pos + 1, end - pos - 1);
}

bool json_number(
    const string& json,
    const string& key,
    double& value,
    string& raw
) {
    string token = "\"" + key + "\"";

    size_t pos = json.find(token);

    if (pos == string::npos) return false;

    pos = json.find(':', pos + token.size());

    if (pos == string::npos) return false;

    pos = json.find_first_not_of(
        " \t\r\n",
        pos + 1
    );

    if (pos == string::npos) return false;

    size_t end = pos;

    while (
        end < json.size() &&
        (
            isdigit(
                static_cast<unsigned char>(json[end])
            ) ||
            json[end] == '-' ||
            json[end] == '+' ||
            json[end] == '.' ||
            json[end] == 'e' ||
            json[end] == 'E'
        )
    ) {
        ++end;
    }

    raw = json.substr(pos, end - pos);

    if (raw.empty()) return false;

    try {
        size_t used = 0;

        value = stod(raw, &used);

        return used == raw.size();
    } catch (...) {
        return false;
    }
}

bool has_active_alert(
    sqlite3 *db,
    const string& sensor_id,
    const string& alert_type
) {
    const char *sql =
        "SELECT 1 FROM alerts "
        "WHERE sensor_id=? "
        "AND alert_type=? "
        "AND status='ACTIVE' LIMIT 1";

    sqlite3_stmt *stmt = nullptr;
    bool found = false;

    if (
        sqlite3_prepare_v2(
            db,
            sql,
            -1,
            &stmt,
            nullptr
        ) == SQLITE_OK
    ) {
        sqlite3_bind_text(
            stmt,
            1,
            sensor_id.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        sqlite3_bind_text(
            stmt,
            2,
            alert_type.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        found = sqlite3_step(stmt) == SQLITE_ROW;
    }

    if (stmt) sqlite3_finalize(stmt);

    return found;
}

void resolve_alerts(
    sqlite3 *db,
    const string& sensor_id,
    const string& keep_type = ""
) {
    string sql =
        "UPDATE alerts SET status='RESOLVED' "
        "WHERE sensor_id=? AND status='ACTIVE'";

    if (!keep_type.empty())
        sql += " AND alert_type<>?";

    sqlite3_stmt *stmt = nullptr;

    if (
        sqlite3_prepare_v2(
            db,
            sql.c_str(),
            -1,
            &stmt,
            nullptr
        ) == SQLITE_OK
    ) {
        sqlite3_bind_text(
            stmt,
            1,
            sensor_id.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        if (!keep_type.empty()) {
            sqlite3_bind_text(
                stmt,
                2,
                keep_type.c_str(),
                -1,
                SQLITE_TRANSIENT
            );
        }

        sqlite3_step(stmt);
    }

    if (stmt) sqlite3_finalize(stmt);
}

void create_alert(
    sqlite3 *db,
    const Sensor& sensor,
    const string& sensor_name,
    const string& value,
    const string& alert_type
) {
    resolve_alerts(
        db,
        sensor.id,
        alert_type
    );

    if (
        has_active_alert(
            db,
            sensor.id,
            alert_type
        )
    ) {
        return;
    }

    const char *sql =
        "INSERT INTO alerts("
        "sensor_id,"
        "sensor_name,"
        "alert_type,"
        "sensor_value,"
        "created_at,"
        "status"
        ") VALUES("
        "?,?,?,?,"
        "datetime('now','localtime'),"
        "'ACTIVE'"
        ")";

    sqlite3_stmt *stmt = nullptr;

    if (
        sqlite3_prepare_v2(
            db,
            sql,
            -1,
            &stmt,
            nullptr
        ) == SQLITE_OK
    ) {
        sqlite3_bind_text(
            stmt,
            1,
            sensor.id.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        sqlite3_bind_text(
            stmt,
            2,
            sensor_name.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        sqlite3_bind_text(
            stmt,
            3,
            alert_type.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        sqlite3_bind_text(
            stmt,
            4,
            value.c_str(),
            -1,
            SQLITE_TRANSIENT
        );

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            cout
                << "[ALERT] Sensor "
                << sensor.id
                << " | "
                << alert_type
                << " | value="
                << value
                << endl;
        }
    }

    if (stmt) sqlite3_finalize(stmt);
}

void check_sensor(
    sqlite3 *db,
    const Sensor& sensor,
    long timeout_seconds,
    double temperature_max,
    double humidity_min,
    double humidity_max,
    double co2_max,
    double smoke_max
) {
    string response =
        fetch_sensor(sensor, timeout_seconds);

    if (
        response.empty() ||
        response.find("\"error\"") != string::npos
    ) {
        create_alert(
            db,
            sensor,
            sensor.name,
            "N/A",
            "NO_DATA"
        );

        return;
    }

    string sensor_name =
        json_string(response, "sensor_name");

    if (sensor_name.empty())
        sensor_name = sensor.name;

    double value = 0.0;
    string raw_value;

    if (
        !json_number(
            response,
            "value",
            value,
            raw_value
        )
    ) {
        create_alert(
            db,
            sensor,
            sensor_name,
            "INVALID",
            "INVALID_VALUE"
        );

        return;
    }

    string alert_type;

    if (
        sensor.type == "temperature" &&
        value > temperature_max
    ) {
        alert_type = "HIGH_TEMPERATURE";
    } else if (
        sensor.type == "humidity" &&
        value < humidity_min
    ) {
        alert_type = "LOW_HUMIDITY";
    } else if (
        sensor.type == "humidity" &&
        value > humidity_max
    ) {
        alert_type = "HIGH_HUMIDITY";
    } else if (
        sensor.type == "co2" &&
        value > co2_max
    ) {
        alert_type = "HIGH_CO2";
    } else if (
        sensor.type == "smoke" &&
        value > smoke_max
    ) {
        alert_type = "SMOKE_DETECTED";
    }

    if (alert_type.empty()) {
        resolve_alerts(db, sensor.id);

        cout
            << "[OK] Sensor "
            << sensor.id
            << " | value="
            << raw_value
            << endl;
    } else {
        create_alert(
            db,
            sensor,
            sensor_name,
            raw_value,
            alert_type
        );
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr
            << "Usage: "
            << argv[0]
            << " <CONFIG_FILE> [--once]"
            << endl;

        return 1;
    }

    if (!load_config(argv[1])) {
        cerr
            << "[Alert] Cannot open config file: "
            << argv[1]
            << endl;

        return 1;
    }

    const char *required[] = {
        "MASTER_IP",
        "MASTER_PORT",
        "ALERT_DB",
        "ALERT_DATA_DIR",
        "ALERT_CHECK_INTERVAL",
        "ALERT_HTTP_TIMEOUT",
        "ALERT_TEMPERATURE_MAX",
        "ALERT_HUMIDITY_MIN",
        "ALERT_HUMIDITY_MAX",
        "ALERT_CO2_MAX",
        "ALERT_SMOKE_MAX"
    };

    for (const char *key : required) {
        if (config[key].empty()) {
            cerr
                << "[Alert] Missing config value: "
                << key
                << endl;

            return 1;
        }
    }

    int interval;
    long timeout_seconds;

    double temperature_max;
    double humidity_min;
    double humidity_max;
    double co2_max;
    double smoke_max;

    try {
        interval =
            stoi(config["ALERT_CHECK_INTERVAL"]);

        timeout_seconds =
            stol(config["ALERT_HTTP_TIMEOUT"]);

        temperature_max =
            stod(config["ALERT_TEMPERATURE_MAX"]);

        humidity_min =
            stod(config["ALERT_HUMIDITY_MIN"]);

        humidity_max =
            stod(config["ALERT_HUMIDITY_MAX"]);

        co2_max =
            stod(config["ALERT_CO2_MAX"]);

        smoke_max =
            stod(config["ALERT_SMOKE_MAX"]);
    } catch (...) {
        cerr
            << "[Alert] Invalid numeric configuration value."
            << endl;

        return 1;
    }

    vector<Sensor> sensors =
        load_sensors(config["ALERT_DATA_DIR"]);

    if (sensors.empty()) {
        cerr
            << "[Alert] No sensors found in "
            << config["ALERT_DATA_DIR"]
            << endl;

        return 1;
    }

    sqlite3 *db = nullptr;

    if (
        sqlite3_open(
            config["ALERT_DB"].c_str(),
            &db
        ) != SQLITE_OK
    ) {
        cerr
            << "[Alert] Cannot open alert database: "
            << config["ALERT_DB"]
            << endl;

        if (db) sqlite3_close(db);

        return 1;
    }

    sqlite3_busy_timeout(db, 3000);

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS alerts("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sensor_id TEXT,"
        "sensor_name TEXT,"
        "alert_type TEXT,"
        "sensor_value TEXT,"
        "created_at TEXT,"
        "status TEXT"
        ")";

    sqlite3_exec(
        db,
        create_sql,
        nullptr,
        nullptr,
        nullptr
    );

    curl_global_init(CURL_GLOBAL_DEFAULT);

    bool once =
        argc >= 3 &&
        string(argv[2]) == "--once";

    cout
        << "[Alert] Monitoring "
        << sensors.size()
        << " sensors every "
        << interval
        << " seconds."
        << endl;

    do {
        for (const Sensor& sensor : sensors) {
            check_sensor(
                db,
                sensor,
                timeout_seconds,
                temperature_max,
                humidity_min,
                humidity_max,
                co2_max,
                smoke_max
            );
        }

        if (!once) {
            this_thread::sleep_for(
                chrono::seconds(interval)
            );
        }
    } while (!once);

    curl_global_cleanup();
    sqlite3_close(db);

    return 0;
}