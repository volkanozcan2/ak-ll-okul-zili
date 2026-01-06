# Akıllı Okul Zili  
**ESP32 + DFPlayer Mini + RTC (DS3231)**  
**Tema:** Algoritma Tasarımı ve Uygulamaları

Bu proje; zaman okuma, karşılaştırma, koşul kontrolü ve karar verme adımlarını içeren bir algoritma kullanarak çalışan **akıllı okul zili sistemi**dir. Sistem, internet bağlantısı olsun ya da olmasın doğru zamanda çalışacak şekilde tasarlanmıştır. İdareciler gerektiğinde manuel olarak müzik veya anons da başlatabilir.

---

## Özellikler
- RTC (DS3231) ile **offline çalışma**
- Wi-Fi varsa **NTP ile otomatik saat güncelleme**
- Algoritma tabanlı **zil çizelgesi**
- DFPlayer Mini ile **MP3 çalma**
- Web arayüzü (HTTP API) üzerinden:
  - Manuel müzik / anons başlatma
  - Zil durdurma
  - Ses seviyesi ayarlama
  - Sistem durumu görüntüleme
- Manuel kullanım sırasında **otomatik zilin geçici olarak kilitlenmesi**

---

## Kullanılan Donanımlar
- ESP32
- DFPlayer Mini MP3 Modülü
- RTC DS3231
- microSD Kart
- Hoparlör / Amfi
- 5V Güç Kaynağı

---

## Donanım Bağlantıları

### RTC DS3231 (I2C)
| RTC | ESP32 |
|---|---|
| SDA | GPIO 21 |
| SCL | GPIO 22 |
| VCC | 3.3V |
| GND | GND |

### DFPlayer Mini (UART)
| DFPlayer | ESP32 |
|---|---|
| TX | GPIO 16 (RX2) |
| RX | GPIO 17 (TX2) *(1K seri direnç önerilir)* |
| VCC | 5V |
| GND | GND |

---

## Yazılım Yapısı (Algoritma)
1. RTC’den anlık tarih ve saat okunur  
2. Wi-Fi varsa NTP üzerinden saat güncellenir ve RTC’ye yazılır  
3. RTC saati, zil çizelgesi ile karşılaştırılır  
4. Koşullar sağlanıyorsa ilgili MP3 dosyası çalınır  
5. Manuel müzik/anons açıldığında otomatik algoritma geçici olarak durdurulur  

---

## Zil Çizelgesi
Zil programı kod içinde aşağıdaki yapı ile tanımlanır:

```cpp
struct BellEvent {
  uint8_t dowFrom;
  uint8_t dowTo;
  uint8_t hour;
  uint8_t minute;
  uint16_t track;
  const char* label;
};
