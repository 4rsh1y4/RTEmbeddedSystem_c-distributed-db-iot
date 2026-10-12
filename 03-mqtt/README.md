<div dir="rtl">

# سیستم توزیع‌شده IoT — بخش سوم: MQTT

**نام:** عرشیا نوروزی  
**شماره دانشجویی:** 402170562

در بخش سوم، پروتکل MQTT به سیستم اضافه شده است. نود Master درخواست‌ها را از طریق Topic درخواست دریافت می‌کند و پاسخ نهایی را با QoS 1 روی Topic پاسخ منتشر می‌کند. لایه‌های SQLite و Memcached بخش قبل نیز حفظ شده‌اند.

---

## پیش‌نیازها

<div dir="ltr" align="left">

```bash
sudo apt update
sudo apt install -y build-essential sqlite3 libsqlite3-dev memcached libmemcached-dev mosquitto mosquitto-clients libmosquitto-dev curl netcat-openbsd
```

</div>

روی هر سه ماشین:

<div dir="ltr" align="left">

```bash
sudo systemctl enable --now memcached
```

</div>

روی Master:

<div dir="ltr" align="left">

```bash
sudo cp 03/mosquitto.conf /etc/mosquitto/conf.d/iot_project.conf
sudo systemctl enable --now mosquitto
```

</div>

---

## تنظیمات MQTT

تنظیمات زیر در فایل `config.txt` ریشه پروژه قرار دارند:

<div dir="ltr" align="left">

```ini
MQTT_BROKER_HOST=127.0.0.1
MQTT_BROKER_PORT=1883
MQTT_REQUEST_TOPIC=sensor/request
MQTT_RESPONSE_TOPIC=sensor/response
MQTT_QOS=1
```

</div>

قالب پیام درخواست:

<div dir="ltr" align="left">

```text
sensor_type|sensor_id
```

</div>

مثال:

<div dir="ltr" align="left">

```text
temperature|101
co2|204
smoke|304
```

</div>

---

## اجرای سیستم

اسکریپت زیر را ابتدا روی Slave 1، سپس Slave 2 و در نهایت روی Master اجرا کنید:

<div dir="ltr" align="left">

```bash
cd ~/IoT_Distributed_Project
chmod +x 03/scripts/*.sh
./03/scripts/build_and_run.sh
```

</div>

این اسکریپت نقش ماشین را از روی IP تشخیص داده، دیتابیس و Memcached را آماده کرده، برنامه را کامپایل و اجرا می‌کند.

---

## تست دستی MQTT

در ترمینال اول روی Master:

<div dir="ltr" align="left">

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -t "sensor/response" -q 1
```

</div>

در ترمینال دوم:

<div dir="ltr" align="left">

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t "sensor/request" -m "temperature|101" -q 1
```

</div>

نمونه درخواست برای Slave 1 و Slave 2:

<div dir="ltr" align="left">

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t "sensor/request" -m "co2|204" -q 1
mosquitto_pub -h 127.0.0.1 -p 1883 -t "sensor/request" -m "smoke|304" -q 1
```

</div>

پاسخ به‌صورت JSON شامل اطلاعات سنسور و فیلد `response_time_ms` منتشر می‌شود.

---

## اجرای Benchmark

<div dir="ltr" align="left">

```bash
cd ~/IoT_Distributed_Project
./03/scripts/benchmark_mqtt.sh
```

</div>

اسکریپت ابتدا کش هر سه نود را خالی می‌کند و سپس تمام سنسورها را در دو دور آزمایش می‌کند:

- دور اول: `Cache Miss` و خواندن از SQLite
- دور دوم: `Cache Hit` و خواندن از Memcached

در پایان، میانگین زمان پاسخ هر دور نمایش داده می‌شود.

گزارش کامل‌تر در فایل `report.md` قرار دارد.

</div>