# Distributed IoT Sensor Network
**Developer:** Arshia Noroozi | **Student ID:** 402170562

## معرفی پروژه
این پروژه یک سیستم پایگاه‌داده توزیع‌شده (Distributed Database) برای مدیریت داده‌های سنسورهای اینترنت اشیا (IoT) است. سیستم با زبان ++C پیاده‌سازی شده و از کتابخانه `mongoose` برای ارتباطات شبکه (HTTP Async Routing) و `sqlite3` برای ذخیره‌سازی داده‌ها بهره می‌برد. 

معماری شبکه شامل یک نود مرکزی (Master) و چند نود کارگر (Slave) است. نود Master وظیفه پاسخگویی به کلاینت و مسیریابی هوشمند درخواست‌ها در شبکه را بر عهده دارد.

---

## پیش‌نیازهای اجرا
برای اجرای کدها روی ماشین‌های مجازی (لینوکس)، نصب بودن پکیج‌های زیر الزامی است:

```bash
sudo apt update
sudo apt install -y build-essential sqlite3 libsqlite3-dev
```

---

## راهنمای اجرا 

### ۱. دریافت پروژه
ابتدا مخزن را روی تمامی ماشین‌های مجازی (Master و Slaveها) کلون کنید:

```bash
git clone https://github.com/4rsh1y4/RTEmbeddedSystem_c-distributed-db-iot.git
cd RTEmbeddedSystem_c-distributed-db-iot/01
```

### ۲. تنظیمات شبکه (فایل کانفیگ)
آدرس‌های IP ماشین‌های مجازی ممکن است تغییر کنند. وارد پوشه `master` شده و فایل `config.txt` را با آدرس‌های IP فعلی شبکه خود تنظیم کنید:

```text
MASTER_IP=192.168.x.x
MASTER_PORT=8080
MASTER_DB=master.db

SLAVE1_URL=http://192.168.x.y:8001
SLAVE2_URL=http://192.168.x.z:8002
```

### ۳. اجرای نودهای Slave (کارگرها)
روی هر کدام از ماشین‌های Slave، وارد پوشه `slave` شده، کدها را کامپایل و سرویس را اجرا کنید:

**برای Slave 1:**
```bash
cd slave
make
./slave_node <SLAVE1_IP> 8001 slave1.db
```

**برای Slave 2:**
```bash
cd slave
make
./slave_node <SLAVE2_IP> 8002 slave2.db
```

### ۴. اجرای نود Master (مرکزی)
روی ماشین اصلی، وارد پوشه `master` شده و نود مرکزی را با پاس دادن فایل کانفیگ اجرا کنید:

```bash
cd master
make
./master_node config.txt
```

---

## تست و ارزیابی شبکه

پس از بالا آمدن تمام نودها، می‌توانید مکانیزم مسیریابی (Routing) را با ارسال درخواست به IP ماشین Master بررسی کنید. پایگاه‌های داده (`.db`) به همراه پروژه قرار داده شده‌اند و شامل داده‌های اولیه هستند.

**تست ۱: واکشی داده‌ی موجود در خودِ Master (بدون پرش شبکه‌ای)**
```bash
curl -s "http://<MASTER_IP>:8080/api/sensor?id=101"
```

**تست ۲: واکشی داده از Slave 1 (مسیریابی اتوماتیک)**
```bash
curl -s "http://<MASTER_IP>:8080/api/sensor?id=204"
```

**تست ۳: واکشی داده از Slave 2**
```bash
curl -s "http://<MASTER_IP>:8080/api/sensor?id=304"
```

**تست ۴: داده‌ی ناموجود (بررسی عملکرد Error Handling)**
```bash
curl -s "http://<MASTER_IP>:8080/api/sensor?id=999"
```

> **نکته:** در حین اجرای تست‌های بالا، ترمینال ماشین Master را مشاهده کنید تا لاگ‌های مربوط به جستجوی نامتقارن (Async HTTP Requests) در شبکه را به صورت زنده ببینید.