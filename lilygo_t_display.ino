// Recuperation des horaires des prochains passages de bus a un arret en Ile de France
// Les informations sont recuperees de la plateform PRIM geree par IdFM 
// Affichage de ces informations sur un LilyGo T-Display
// https://tutoduino.fr/
// Copyleft 2025

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"
#include "line_mapping.h"

// Broches du ST7789 sur le LilyGo T-Display
#define TFT_MOSI 19
#define TFT_SCLK 18
#define TFT_CS 5
#define TFT_DC 16
#define TFT_RST 23
#define TFT_BL 4

// Objet sur l'ecran du LilyGo T-Display
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// Nombre de prochains passages a afficher
#define NB_BUS_SCHEDULE 3

// Structure pour stocker les informations sur les prochains passages
struct BusSchedule {
  String line;
  String time;
};

// URL du service, il doit etre completee par l'identifiant de l'arret de bus qui est stocke dans "secrets.h"
const char* service_url = "https://prim.iledefrance-mobilites.fr/marketplace/stop-monitoring?MonitoringRef=";

// ---------------------------------------------------------------------------
//  Synchronisation de l'heure NTP + fuseau France (DST auto)
// ---------------------------------------------------------------------------
void setupTime() {
  // Fuseau France normalisé : CET-1CEST,M3.5.0/2,M10.5.0/3
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");
  Serial.println("Heure synchronisée (France)");
}

// ---------------------------------------------------------------------------
//  Convertit l'heure UTC en heure locale, le format de l'heure étant une
//  chaîne de caractères « HH:MM »
// ---------------------------------------------------------------------------
String convertUTCtoLocal(String utcTime) {
  struct tm local_tm = { 0 };
  char buffer[6];
  int h = utcTime.substring(0, 2).toInt();
  int m = utcTime.substring(3, 5).toInt();

  // Recuperation de l'heure locale en UTC
  if (!getLocalTime(&local_tm)) {
    Serial.println("Erreur : impossible d'obtenir l'heure locale !");
    return utcTime;  // fallback
  }

  switch (local_tm.tm_isdst) {
    case 0:  // heure d'hiver = UTC + 1
      h += 1;
      break;
    case 1:  // heure d'ete = UTC + 2
      h += 2;
      break;
    default:  // indetermine ou erreur, retourne l'heure UTC
      break;
  }

  // Gestion dépassement 24h
  if (h >= 24) h -= 24;
  if (h < 0) h += 24;

  sprintf(buffer, "%02d:%02d", h, m);
  return String(buffer);
}

// ---------------------------------------------------------------------------
//  Extrait l'heure HH:MM d'une chaîne ISO 8601 "YYYY-MM-DDTHH:MM:SS.000Z"
// ---------------------------------------------------------------------------
String getTimeHHMM(const char* isoString) {
  char timeStr[6];  // HH:MM\0

  // Positions fixes dans ISO 8601
  timeStr[0] = isoString[11];  // H1
  timeStr[1] = isoString[12];  // H2
  timeStr[2] = ':';
  timeStr[3] = isoString[14];  // M1
  timeStr[4] = isoString[15];  // M2
  timeStr[5] = '\0';

  return String(timeStr);
}

// -----------------------------------------------------------------------------
//  Appel de l'API PRIM pour obtenir les horaires des passages des prochains bus
//  Retourne le nombre d'horaires trouves
// -----------------------------------------------------------------------------
int getExpectedDepartureTime(BusSchedule schedules[NB_BUS_SCHEDULE]) {

  // Initialise les NB_BUS_SCHEDULE prochains bus
  for (int i = 0; i < NB_BUS_SCHEDULE; i++) {
    schedules[i].line = "";
    schedules[i].time = "";
  }

  // Verifie que le Wi-Fi est bien connecte
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Wi-Fi not connected.");
    return 0;
  }

  // Contruit la requete HTTP pour l'arret concerne
  HTTPClient http;
  String full_url = String(service_url) + String(stop_point_ref);  
  http.begin(full_url);
  http.addHeader("apikey", api_key);
  http.addHeader("accept", "application/json");

  // Envoir la requete et recupere la reponse (payload)
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
    Serial.println(http.getString());
    http.end();
    return 0;
  }

  String payload = http.getString();
  http.end();

  // La reponse au format JSON peut être volumineuse en fonction du nombre de lignes de bus a cet arret
  DynamicJsonDocument doc(40 * 1024);

  if (deserializeJson(doc, payload, DeserializationOption::NestingLimit(20))) {
    Serial.println("JSON parsing error !");
    return 0;
  }

  JsonArray visits =
    doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]["MonitoredStopVisit"];

  int count = 0;

  // Recupere les informations (ligne de bus et heure de depart) pour les NB_BUS_SCHEDULE prochains passages
  for (JsonObject visit : visits) {
    if (count >= NB_BUS_SCHEDULE) {
      break;
    }
    // Identifiant de la ligne de bus
    const char* lineRef = visit["MonitoredVehicleJourney"]["LineRef"]["value"];
    // Horaire du prochain depart
    const char* expectedDepartureTime =
      visit["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedDepartureTime"];

    if (!lineRef || !expectedDepartureTime) {
      continue;
    }

    // Vérifie que nous souhaitons les informations concernant cette ligne de bus
    bool isKnownLine = false;
    for (int i = 0; i < lineMappingsSize; i++) {
        if (lineMappings[i].ref == String(lineRef)) {
            isKnownLine = true;
            break;
        }
    }

    // Rertourner l'heure du prochain depart pour les lignes de bus identifiees dans le lineMapping
    if (isKnownLine) { 

      schedules[count].line = mapLineRefToNumber(lineRef);;
      schedules[count].time = convertUTCtoLocal(getTimeHHMM(expectedDepartureTime));

      count++;
    }
  }

  return count;
}


// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  delay(1000);

  // Connexion Wi-Fi
  WiFi.begin(wifi_ssid, wifi_password);
  Serial.print("Connexion Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnecté au Wi-Fi");

  setupTime();  // NTP + TZ

  // Allume le rétroéclairage
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Initialisation écran
  tft.init(135, 240);
  tft.setRotation(1);
  tft.setFont(&FreeSansBold18pt7b);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(30, 70);
  tft.write("Tutoduino");
  delay(1000);
}


// ---------------------------------------------------------------------------
//  LOOP PRINCIPALE
// ---------------------------------------------------------------------------
void loop() {

  BusSchedule nextBuses[NB_BUS_SCHEDULE];
  struct tm timeinfo;
  char timeString[6];
  char buf[15];

  tft.fillScreen(ST77XX_BLACK);

  // Affiche l'heure locale
  if (getLocalTime(&timeinfo)) {
    strftime(timeString, sizeof(timeString), "%H:%M", &timeinfo);
    Serial.print("Heure locale: ");
    Serial.println(timeString);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(70, 25);
    tft.print(timeString);
    tft.drawLine(0, 32, 240, 32, ST77XX_WHITE);
  }

  // Récupère les horaires des prochains bus
  int numBuses = getExpectedDepartureTime(nextBuses);

  if (numBuses > NB_BUS_SCHEDULE) {
    Serial.printf("Erreur");
  } else if (numBuses > 0) {
    for (int i = 0; i < numBuses; i++) {
      Serial.printf("Ligne %s  %s\n",
                    nextBuses[i].line,
                    nextBuses[i].time);

      snprintf(buf, sizeof(buf), "%s  %s",
               nextBuses[i].line, nextBuses[i].time);
      tft.setTextColor(ST77XX_CYAN);
      tft.setCursor(0, 65 + 30 * i);
      tft.print(buf);
    }
  } else {
    tft.print("Aucun bus");
  }

  // Réactualise toutes les 60 secondes
  delay(60 * 1000);
}
