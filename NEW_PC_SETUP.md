# New PC Setup Checklist

## Цель

Подготовить новый ПК для работы с проектом `esp32.v1`, склонировать проект из
GitHub и продолжить работу с веткой для обычной ESP32.

## Что установить

1. Git
2. Visual Studio Code
3. Расширение ESP-IDF для VS Code
4. ESP-IDF v5.5.4
5. ESP-IDF tools для target `esp32`

## ESP-IDF

Желательно установить ESP-IDF в тот же путь:

```text
C:\esp\v5.5.4\esp-idf
```

Открой `ESP-IDF 5.5 PowerShell` и выполни:

```powershell
cd C:\esp\v5.5.4\esp-idf
.\install.ps1 esp32
```

Если `idf.py` не находится в обычном терминале, активируй среду:

```powershell
C:\esp\v5.5.4\esp-idf\export.ps1
```

## Клонирование проекта

Перейди в папку для проектов, например:

```powershell
mkdir F:\IDE
cd F:\IDE
```

Склонируй репозиторий:

```powershell
git clone https://github.com/fufilius/esp32.v1.git
cd esp32.v1
```

Переключись на рабочую ветку:

```powershell
git switch feature/esp32-board-prep
```

Проверь ветку:

```powershell
git status -sb
```

Ожидаемый результат:

```text
## feature/esp32-board-prep...origin/feature/esp32-board-prep
```

## Сборка

```powershell
idf.py build
```

Если нужно явно выставить target:

```powershell
idf.py set-target esp32
idf.py build
```

## Прошивка

Подключи ESP32 и найди COM-порт в диспетчере устройств или в VS Code ESP-IDF.

Прошивка и монитор:

```powershell
idf.py -p COMx flash monitor
```

Пример:

```powershell
idf.py -p COM5 flash monitor
```

## Текущая распиновка ESP32

```text
BH1750 VCC  -> 3V3
BH1750 GND  -> GND
BH1750 SDA  -> GPIO21
BH1750 SCL  -> GPIO22
BH1750 ADDR -> GND or not connected

DHT22 VCC   -> 3V3
DHT22 GND   -> GND
DHT22 DATA  -> GPIO27

RGB Red     -> GPIO25
RGB Green   -> GPIO26
RGB Blue    -> GPIO33
```

Если DHT22 не готовый модуль, добавь подтягивающий резистор примерно `10k`
между DATA и `3V3`.

## Что написать Codex на новом ПК

```text
Продолжаем проект. Я на новом ПК, ветка feature/esp32-board-prep.
Нужно проверить среду и продолжить работу с обычной ESP32.
```

## Быстрая диагностика

```powershell
git status -sb
idf.py --version
idf.py build
```

## Важно

Рабочая ветка:

```text
feature/esp32-board-prep
```

Основной проект уже подготовлен под обычную ESP32, а не ESP32-C3.
