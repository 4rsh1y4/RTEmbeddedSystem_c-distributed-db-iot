<div dir="rtl">

# گزارش معماری و طراحی سیستم پایگاه‌داده توزیع‌شده (بخش اول)

**نام و نام خانوادگی:** عرشیا نوروزی (Arshiya Noroozi)  
**شماره دانشجویی:** 402170562  
**محیط توسعه:** C++, Mongoose, SQLite, Linux VMs  

---

## 1. ساختار پایگاه داده
پایگاه‌داده‌ی استفاده شده در تمام گره‌ها `SQLite` است. برای رعایت اصول نرمال‌سازی، داده‌ها در دو جدول مجزا ذخیره شده‌اند:
*   **جدول `sensors`:** حاوی اطلاعات پایه‌ی سنسور (مانند نام، نوع و واحد اندازه‌گیری).
*   **جدول `sensor_readings`:** حاوی مقادیر خوانده‌شده و زمان ثبت آن‌ها.

برای جلوگیری از ارسال درخواست‌های متعدد به دیتابیس، از کوئری ترکیبی `JOIN` استفاده شده است. این کوئری آخرین دیتای ثبت‌شده را بر اساس زمان `recorded_at` استخراج کرده و اطلاعات هویتی سنسور را با مقدار فعلی آن ادغام می‌کند:
<div dir="ltr" align="left">

```sql
SELECT s.sensor_type, s.sensor_id, s.sensor_name, r.value, s.unit, r.recorded_at
FROM sensors s 
JOIN sensor_readings r ON s.sensor_id = r.sensor_id 
WHERE s.sensor_type = ? AND s.sensor_id = ?
ORDER BY r.recorded_at DESC LIMIT 1;
```
</div>

## 1.1 قرارداد درخواست و اعتبارسنجی ورودی
ورودی درخواست از دو جزء مستقل تشکیل شده است: `sensor_type` و `sensor_id`. Master فقط زمانی جستجو را آغاز می‌کند که هر دو پارامتر `type` و `id` موجود و دارای قالب معتبر باشند. هر نود در کوئری SQLite هر دو ستون `sensor_type` و `sensor_id` را بررسی می‌کند؛ بنابراین ارسال شناسه صحیح همراه با نوع اشتباه منجر به بازگرداندن داده نخواهد شد. درخواست ناقص یا دارای نویسه‌های نامعتبر با وضعیت `400 Bad Request` رد می‌شود.


---

## 2. دیاگرام شبکه نودها
در این معماری، گره Master در مرکز قرار داشته و به عنوان API Gateway عمل می‌کند. اپراتور صرفاً با Master در ارتباط است و هیچ دسترسی مستقیمی به گره‌های Slave ندارد. جریان ارتباطات و دیتابیس‌های محلی در دیاگرام زیر به تصویر کشیده شده است:

```mermaid
graph TD
    O[Operator / Client] -->|GET /api/sensor?type=T&id=X| M[Master<br/>192.168.54.134:8080]

    subgraph Master_Server
        M -->|Local SQLite Query| MDB[(master.db)]
    end

    subgraph Slave_1_Server
        M -->|HTTP + X-Master-Token| S1[Slave 1<br/>192.168.54.136:8001]
        S1 --> S1DB[(slave1.db)]
    end

    subgraph Slave_2_Server
        M -->|Fallback HTTP + X-Master-Token| S2[Slave 2<br/>192.168.54.133:8002]
        S2 --> S2DB[(slave2.db)]
    end

    S1 -.->|JSON Response or 404| M
    S2 -.->|JSON Response or 404| M
    M -->|Final JSON Response| O
```


## 2.1 مدیریت پیکربندی شبکه و پایگاه‌داده
تمام مشخصات اجرایی بخش‌های `01` تا `05` در یک فایل مشترک در ریشه پروژه، یعنی `../config.txt`، نگهداری می‌شوند. Master آدرس مقصد Slaveها را در زمان اجرا از ترکیب `SLAVE*_IP` و `SLAVE*_PORT` می‌سازد و اسکریپت راه‌اندازی نیز همان مقادیر را برای Listener هر نود استفاده می‌کند. مسیرهای نسبی دیتابیس در هر بخش نسبت به پوشه همان نود تفسیر می‌شوند؛ بنابراین یک فایل مشترک می‌تواند برای همه بخش‌ها استفاده شود، در حالی که دیتابیس‌های هر بخش مستقل باقی می‌مانند. فایل ریشه‌ای `config.example` قالب تنظیمات را مستند می‌کند.


---

## 3. مسیر درخواست و پاسخ (معماری Asynchronous)
در این سیستم، مسیر درخواست بر اساس معماری **Event-Driven** و غیرهمزمان (Asynchronous) طراحی شده است تا از مسدود شدن سرور (Blocking) جلوگیری شود:
1. اپراتور درخواست را به Master ارسال می‌کند.
2. گره Master ابتدا دیتابیس محلی خود را بررسی می‌کند.
3. در صورت عدم یافتن داده، درخواست از طریق تابع `mg_http_connect` بدون توقف پردازنده به سمت Slave 1 فرستاده و Route می‌شود.
4. اگر Slave 1 پاسخ منفی دهد (یا در صورت قطعی شبکه، خطای `MG_EV_ERROR` رخ دهد)، مکانیزم **Fallback** فعال شده و درخواست بلافاصله به Slave 2 ارسال می‌گردد.
5. پس از آماده شدن پاسخ در Slave، رویداد `MG_EV_HTTP_MSG` در Master به عنوان Trigger عمل کرده و داده استخراج‌شده مستقیماً به اپراتور برگردانده می‌شود. 

این رویکرد علاوه بر جلوگیری از Starvation، تحمل خطای (Fault Tolerance) بالایی در برابر قطعی گره‌ها فراهم کرده است.

---

## 4. سناریوهای ارزیابی و تست شبکه

### 4.1 اجرای اسکریپت تست خودکار

پس از اجرای هر سه نود، اسکریپت تست روی ماشین Master اجرا شد:
<div dir="ltr" align="left">

```bash
cd ~/IoT_Distributed_Project
./01/scripts/test_requests.sh
```
</div>


این اسکریپت حالت‌های خواندن داده از Master، Slave 1، Slave 2، ورودی نامعتبر، سنسور ناموجود و جلوگیری از دسترسی مستقیم به Slave را بررسی می‌کند.

**خلاصه نتیجه تست:**
<div dir="ltr" align="left">

| تست | HTTP مورد انتظار | HTTP واقعی | نتیجه |
|---|---:|---:|---|
| داده موجود در Master | `200` | 200 | "sensor_type": "temperature", "sensor_id": "101", "sensor_name": "Floor1_Room101_Temp", "value": 24.8, "unit": "C", "recorded_at": "2026-06-01 10:15:00" |
| داده موجود در Slave 1 | `200` | 200 | "sensor_type": "temperature", "sensor_id": "201", "sensor_name": "Floor2_Room201_Temp", "value": 25.3, "unit": "C", "recorded_at": "2026-06-01 10:15:00" |
| داده موجود در Slave 2 | `200` | 200 | "sensor_type": "temperature", "sensor_id": "301", "sensor_name": "Floor3_Room301_Temp", "value": 23.1, "unit": "C", "recorded_at": "2026-06-01 10:15:00"|
| نوع اشتباه برای شناسه معتبر | `404` | 404 | "error": "Sensor type and ID not found in the network" |
| سنسور ناموجود | `404` | 404 | "error": "Sensor type and ID not found in the network" |
| نبود پارامتر `type` | `400` | 400 | "error": "Both query parameters 'type' and 'id' are required" |
| دسترسی مستقیم به Slave | `403` | 403 | "error": "Forbidden: requests must come through Master" |
</div>

<div dir="ltr" align="center">

| خروجی اسکریپت تست خودکار |
| :---: |
| <img src="images/part01_test_requests.png" alt="خروجی اسکریپت تست" width="500"> |

---
</div>

---

### 4.2 تست خواندن دستی داده:

با درخواست های دستی زیر داده های زیر را از مستر یا اسلیو خواستیم تا نتایج را ببینیم:

1. سنسور موحجود در مستر:

<div dir="ltr" align="left">

```bash
curl -s "http://192.168.54.134:8080/api/sensor?type=temperature&id=101"
```
</div>

2. سنسور موحجود در اسلیو 1:
<div dir="ltr" align="left">

```bash
curl -s "http://192.168.54.134:8080/api/sensor?type=co2&id=204"
```
</div>

3. سنسور موحجود در اسلیو 2:
<div dir="ltr" align="left">

```bash
curl -s   "http://192.168.54.134:8080/api/sensor?type=smoke&id=304"
```
</div>

4. سنسور با تایپ اشتباه در سیستم:
<div dir="ltr" align="left">

```bash
curl -s   "http://192.168.54.134:8080/api/sensor?type=humidity&id=101"
```
</div>

5. سنسور ناموجود در سیستم:
<div dir="ltr" align="left">

```bash
curl -s   "http://192.168.54.134:8080/api/sensor?type=temperature&id=999"
```
</div>

6. سنسور بدون مشخص کردن تایپ:
<div dir="ltr" align="left">

```bash
curl -s   "http://192.168.54.134:8080/api/sensor?id=101"
```
</div>

7. درخواست سنسور از نود غیر مستر:
<div dir="ltr" align="left">

```bash
curl -s   "http://192.168.54.136:8001/api/sensor?type=co2&id=204"
```
</div>

**نتیجه اجرا:**

| پاسخ‌های Client و لاگ Master | لاگ Slaveها |
| :---: | :---: |
| ![پاسخ‌های Client و لاگ Master](images/multiple_master&client.png) | ![لاگ Slaveها](images/multiple_slaves.png) |
---

### 4.3 تست fault tolerance

ابتدا Slave 1 به صورت عمدی خاموش گردید و درخواست سنسور  304 را که در نود دوم بود با دستور زیر، به مستر ارسال میکنیم:

<div dir="ltr" align="left">

```bash
curl -s "http://192.168.54.134:8080/api/sensor?type=smoke&id=304"
```
</div>

**نتیجه اجرا:**

| خروجی Clientو Master | وضعیت Slaveها |
| :---: | :---: |
| ![Client](images/ft_304_master&client.png) | ![Slaves](images/ft_304_slaves.png) |

لاگ‌ها نشان می‌دهد Master با خطای `socket error` مواجه شده، اما به دلیل مکانیزم **Fallback**، سیستم مستقیماً درخواست را به Slave 2 ارسال کرده است.

---

### 4.4 جمع‌بندی نتایج عملی

پس از اجرای تست‌ها، موارد زیر بر اساس خروجی واقعی تکمیل می‌شوند:

- خواندن محلی از Master: `موفق`
- مسیریابی به Slave 1: `موفق`
- مسیریابی به Slave 2: `موفق`
- تشخیص ورودی ناقص: `موفق`
- تشخیص سنسور ناموجود: `موفق`
- جلوگیری از دسترسی مستقیم به Slave: `موفق`
- ادامه عملکرد در صورت قطع Slave 1: `موفق`
---


## 5. کنترل دسترسی و بررسی امنیت HTTP
برای اجرای قانون «اپراتور فقط با Master ارتباط دارد»، یک کلید مشترک با نام `MASTER_SLAVE_TOKEN` در فایل پیکربندی ریشه تعریف شده است. Master این مقدار را فقط در درخواست‌های داخلی، داخل هدر `X-Master-Token`، برای Slave ارسال می‌کند. Slave قبل از پردازش URI و دسترسی به SQLite، هدر را با مقدار پیکربندی‌شده مقایسه می‌کند. درخواست مستقیم اپراتور که این هدر معتبر را ندارد با وضعیت `403 Forbidden` رد می‌شود. اسکریپت تست نیز این سناریو را بررسی می‌کند.

این کنترل، دسترسی مستقیم تصادفی یا بدون مجوز را در لایه برنامه مسدود می‌کند، اما جایگزین رمزنگاری نیست؛ زیرا HTTP خام هدر و داده را به صورت Plain Text منتقل می‌کند. برای استقرار واقعی باید علاوه بر توکن، از HTTPS/TLS استفاده شود و Firewall نودهای Slave فقط اتصال از IP نود Master را مجاز کند. همچنین کلید نمونه باید با مقدار تصادفی طولانی تعویض و خارج از مخزن عمومی نگهداری شود.

</div>