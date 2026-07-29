<div dir="rtl">  
  
# IoT Distributed System - Part 4: SNMP Monitoring  
**Author:** Arshiya Noroozi   
**Student ID:** 402170562  
  
## 1. نحوه نصب بسته‌های SNMP  
برای راه‌اندازی این بخش، به دیمن اصلی SNMP (`snmpd`) و ابزارهای کلاینت آن (`snmp`) نیاز است. نصب این بسته‌ها در توزیع‌های اوبونتو با دستور زیر انجام می‌شود:  

<div dir="ltr" align="left">

```bash
sudo apt update
sudo apt install -y snmpd snmp
```

</div>
  
## 2. نحوه پیکربندی snmpd  
تنظیمات این سرویس در فایل `/etc/snmp/snmpd.conf` قرار می‌گیرد. ما از قابلیت `pass` جهت اتصال OIDهای سفارشی به اسکریپت Bash استفاده کردیم.

ابتدا اسکریپت واسط و فایل‌های داده را به‌صورت دستی در مسیر `/opt/iot_project/` کپی کنید:

<div dir="ltr" align="left">

```bash
cd ~/IoT_Distributed_Project

sudo install -d -m 755 /opt/iot_project
sudo install -d -m 755 /opt/iot_project/data

sudo install -m 755 04/snmp/snmp_extension.sh /opt/iot_project/snmp_extension.sh
sudo install -m 644 04/data/*.csv /opt/iot_project/data/
```

</div>

سپس اطلاعات Master و OID پایه را از فایل `config.txt` خوانده و فایل تنظیمات موردنیاز اسکریپت را ایجاد کنید:

<div dir="ltr" align="left">

```bash
MASTER_IP=$(grep '^MASTER_IP=' config.txt | cut -d '=' -f2 | tr -d ' \r')
MASTER_PORT=$(grep '^MASTER_PORT=' config.txt | cut -d '=' -f2 | tr -d ' \r')
SNMP_BASE_OID=$(grep '^SNMP_BASE_OID=' config.txt | cut -d '=' -f2 | tr -d ' \r')

sudo tee /opt/iot_project/snmp_runtime.conf >/dev/null <<EOF
MASTER_IP=$MASTER_IP
MASTER_PORT=$MASTER_PORT
SNMP_BASE_OID=$SNMP_BASE_OID
SNMP_DATA_DIR=/opt/iot_project/data
EOF
```

</div>

در نهایت فایل کانفیگ آماده پروژه را در مسیر تنظیمات `snmpd` قرار دهید:

<div dir="ltr" align="left">

```bash
sudo install -m 644 04/snmp.conf /etc/snmp/snmpd.conf
```

</div>

خط‌های اصلی کانفیگ به این شکل هستند:

<div dir="ltr" align="left">

```conf
rocommunity public 127.0.0.1
pass .1.3.6.1.4.1.402170562 /opt/iot_project/snmp_extension.sh
```

</div>
  
## 3. ساختار OID های تعریف‌شده  
برای رعایت استانداردهای SNMP، از شماره دانشجویی 402170562 به عنوان Enterprise ID اختصاصی پروژه استفاده شده است:  
* Base OID: .1.3.6.1.4.1.402170562  
* Sensor ID: .{id} (مثلاً 101, 204, 301)  
* Attributes:  
  * .1 : نام سنسور (Sensor Name)  
  * .2 : توضیحات و واحد سنسور (Description / Unit)  
  * .3 : آخرین مقدار ذخیره‌شده سنسور (Value)  
  
## 4. نحوه اجرای سرویس SNMP  
پس از اعمال تنظیمات جدید، سرویس SNMP با دستورات زیر فعال و راه‌اندازی مجدد می‌شود:

<div dir="ltr" align="left">

```bash
sudo systemctl enable --now snmpd
sudo systemctl restart snmpd
systemctl is-active snmpd
```

</div>

خروجی دستور آخر باید `active` باشد.
  
  ## ۵. تست یک سنسور با SNMP

در یک ترمینال جدید روی Master، سه ویژگی سنسور `101` را با دستور زیر بخوانید:

<div dir="ltr" align="left">

```bash
snmpget -v2c -c public -On udp:127.0.0.1:161 \
.1.3.6.1.4.1.402170562.101.1 \
.1.3.6.1.4.1.402170562.101.2 \
.1.3.6.1.4.1.402170562.101.3
```

</div>

برای هر سنسور سه OID در نظر گرفته شده است. در این خروجی، نام سنسور، توضیحات سنسور و آخرین مقدار ذخیره‌شده نمایش داده می‌شوند.

خروجی باید تقریباً شامل موارد زیر باشد:

<div dir="ltr" align="left">

```text
.1.3.6.1.4.1.402170562.101.1 = STRING: "Floor1_Room101_Temp"
.1.3.6.1.4.1.402170562.101.2 = STRING: "Type: temperature | Location: ... | Unit: C"
.1.3.6.1.4.1.402170562.101.3 = STRING: "24.8"
```

</div>

این سه مقدار به‌ترتیب نشان‌دهنده نام سنسور، توضیحات سنسور و آخرین مقدار ذخیره‌شده آن هستند.

## 6. نحوه خواندن اطلاعات با snmpwalk  
برای خواندن مقادیر کل شاخه به صورت بازگشتی، از ابزار `snmpwalk` استفاده می‌شود:

<div dir="ltr" align="left">

```bash
snmpwalk -v2c -c public -On udp:127.0.0.1:161 .1.3.6.1.4.1.402170562
```

</div>
  
## 7. نحوه اجرای اسکریپت تست  
به منظور سهولت در تست، یک اسکریپت خودکار توسعه داده شده است. پس از اجرای سیستم با `build_and_run.sh`، در یک ترمینال جدید دستور زیر را اجرا کنید:

<div dir="ltr" align="left">

```bash
cd ~/IoT_Distributed_Project
chmod +x 04/scripts/test_snmp.sh
./04/scripts/test_snmp.sh
```

</div>
  
## 8. توضیح مسیر دریافت داده (از پایگاه داده تا خروجی SNMP)  
معماری این سیستم به گونه‌ای است که کپسوله‌سازی و یکپارچگی شبکه حفظ شود:  
1. دستور `snmpwalk` توسط کاربر اجرا می‌شود.  
2. دیمن `snmpd` با دیدن OID سفارشی، اسکریپت `/opt/iot_project/snmp_extension.sh` را فرامی‌خواند.  
3. اسکریپت Bash مستقیماً به دیتابیس وصل نمی‌شود؛ بلکه یک درخواست HTTP GET به API نود Master (در پورت 8080) می‌فرستد.  
4. نود Master درخواست را پردازش کرده و مقادیر را از سیستم کشینگ (Memcached) یا پایگاه‌های داده لوکال/ریموت خوانده و به صورت JSON بازمی‌گرداند.  
5. اسکریپت Bash خروجی JSON را با ابزارهای بومی پارس کرده و به فرمت رشته‌ای استاندارد چاپ می‌کند تا `snmpd` آن را به کاربر نمایش دهد.  

</div>