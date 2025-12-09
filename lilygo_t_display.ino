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
#include <esp_sleep.h>
#include "secrets.h"
#include "line_mapping.h"

// Bouton qui permet de sortir de veille profonde
#define BUTTON_PIN 35

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
#define NB_BUS_DISPLAYED 3
// Nombre de prochains passages à lire dans la réponse du serveur PRIM
#define MAX_NB_BUS_SCHEDULE 6

// Structure pour stocker l'heure
struct myTime_t {
  int heure;
  int minute;
};

// Structure pour stocker les informations sur les prochains passages
struct busSchedule_t {
  String busLine;
  myTime_t busTime;
};

// URL du service, il doit etre completee par l'identifiant de l'arret de bus qui est stocke dans "secrets.h"
const char* serviceUrl = "https://prim.iledefrance-mobilites.fr/marketplace/stop-monitoring?MonitoringRef=";

// Variable globale utilisée pour la mise en veille
uint8_t loopCounter;


// ---------------------------------------------------------------------------
//  Vérifie si myTime_t > struct tm (seulement heure/minute)
// ---------------------------------------------------------------------------
bool isMyTimeGreater(myTime_t time1, tm time2) {
  // Compare heures
  if (time1.heure != time2.tm_hour) {
    return time1.heure > time2.tm_hour;
  }

  // Heures égales → compare minutes
  return time1.minute > time2.tm_min;
}

// ---------------------------------------------------------------------------
//  Synchronisation de l'heure NTP + fuseau France (DST auto)
// ---------------------------------------------------------------------------
void setupTime() {
  // Fuseau France normalisé : CET-1CEST,M3.5.0/2,M10.5.0/3
  configTzTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");
  Serial.println("Heure synchronisée (France)");
}

// ---------------------------------------------------------------------------
//  Convertit l'heure UTC en heure locale
// ---------------------------------------------------------------------------
myTime_t convertUTCtoLocal(myTime_t utcTime) {
  struct tm localTime = { 0 };
  char buffer[6];
  myTime_t myLocalTime;

  // Recuperation de l'heure locale en UTC pour vérifier si
  // nous sommes en heures d'hiver ou heure d'ete (isdst)
  if (!getLocalTime(&localTime)) {
    Serial.println("Erreur : impossible d'obtenir l'heure locale !");
    return utcTime;  // fallback
  }

  myLocalTime = utcTime;

  switch (localTime.tm_isdst) {
    case 0:  // heure d'hiver = UTC + 1
      myLocalTime.heure += 1;
      break;
    case 1:  // heure d'ete = UTC + 2
      myLocalTime.heure += 2;
      break;
    default:  // indetermine ou erreur, retourne l'heure UTC
      break;
  }

  // Gestion dépassement 24h
  if (myLocalTime.heure >= 24) {
    myLocalTime.heure -= 24;
  }
  if (myLocalTime.heure < 0) {
    myLocalTime.heure += 24;
  }

  return myLocalTime;
}

// ---------------------------------------------------------------------------
//  Extrait l'heure HH:MM d'une chaîne ISO 8601 "YYYY-MM-DDTHH:MM:SS.000Z"
// ---------------------------------------------------------------------------
myTime_t getTimeHHMM(const char* isoString) {

  myTime_t myTime;

  // JJ et MM sont a des positions fixes dans ISO 8601
  myTime.heure = (isoString[11] - '0') * 10 + (isoString[12] - '0');
  myTime.minute = (isoString[14] - '0') * 10 + (isoString[15] - '0');

  return myTime;
}

// -----------------------------------------------------------------------------
//  Appel de l'API PRIM pour obtenir les horaires des passages des prochains bus
//  Retourne le nombre d'horaires contenant des informations sur les lignes
//  qui nous interessent.
// -----------------------------------------------------------------------------
int getExpectedDepartureTime(busSchedule_t schedules[MAX_NB_BUS_SCHEDULE]) {

  // Nombre d'horaire retourné
  int nbScheduleInfo = 0;

  // Initialise les MAX_NB_BUS_SCHEDULE prochains bus
  for (int i = 0; i < MAX_NB_BUS_SCHEDULE; i++) {
    schedules[i].busLine = "";
    schedules[i].busTime.heure = 0;
    schedules[i].busTime.minute = 0;
  }

  // Verifie que le Wi-Fi est bien connecte
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Wi-Fi not connected.");
    return 0;
  }

  // Contruit la requete HTTP pour l'arret concerne
  HTTPClient http;
  String fullUrl = String(serviceUrl) + String(stopPointRef);
  http.begin(fullUrl);
  http.addHeader("apikey", apiKey);
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

  Serial.println(payload);

  // La reponse au format JSON peut être volumineuse en fonction du nombre de lignes de bus a cet arret
  DynamicJsonDocument doc(40 * 1024);

  if (deserializeJson(doc, payload, DeserializationOption::NestingLimit(20))) {
    Serial.println("JSON parsing error !");
    return 0;
  }

  // Tableau de N elements contenant les informations sur le passage
  // dont le numero de la ligne et l'heure de départ
  JsonArray visits =
    doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]["MonitoredStopVisit"];


  // Recupere les informations (ligne de bus et heure de depart) pour les
  // prochains passages, en se limiant à MAX_NB_BUS_SCHEDULE elements
  for (JsonObject visit : visits) {
    if (nbScheduleInfo >= MAX_NB_BUS_SCHEDULE) {
      Serial.println("Maximum number of information collected, skip following ones");
      break;
    }
    // Identifiant de la ligne de bus
    const char* lineRef = visit["MonitoredVehicleJourney"]["LineRef"]["value"];
    // Horaire du prochain depart
    const char* expectedDepartureTime =
      visit["MonitoredVehicleJourney"]["MonitoredCall"]["ExpectedDepartureTime"];

    if (!lineRef || !expectedDepartureTime) {
      Serial.println("No data");
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

    // Rertourner l'heure du prochain depart et le numero de la ligne si cette ligne
    // nous interesse
    if (isKnownLine) {
      schedules[nbScheduleInfo].busLine = mapLineRefToNumber(lineRef);
      schedules[nbScheduleInfo].busTime = convertUTCtoLocal(getTimeHHMM(expectedDepartureTime));
      nbScheduleInfo++;
    }
  }

  return nbScheduleInfo;
}

// -----------------------------------------------------------------------------
//  Mise en veille profonde de l'ESP32 et de l'ecran LCD
// -----------------------------------------------------------------------------
void enterDeepSleep() {
  // Configure la sortie de veille par appui sur bouton
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, LOW);

  // Éteindre écran et le mettre en veille
  digitalWrite(TFT_BL, LOW);
  tft.writeCommand(0x10);

  // Activer la veille profonde du microcontrolleur
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {

  Serial.begin(115200);
  delay(1000);

  // Le bouton est une entrée qui permet de sortir de veille
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Connexion Wi-Fi
  WiFi.begin(wifiSsid, wifiPassword);
  Serial.print("Connexion Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnecté au Wi-Fi");

  // Configure la recuperation de l'heure et le fuseau horaire
  setupTime();  // NTP + TZ

  // Allume le rétroéclairage
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Initialisation écran
  tft.init(135, 240);
  tft.setRotation(1);
  tft.setFont(&FreeSansBold18pt7b);
  delay(1000);

  loopCounter = 0;
}


// ---------------------------------------------------------------------------
//  BOUCLE PRINCIPALE
// ---------------------------------------------------------------------------
void loop() {

  busSchedule_t nextBuses[MAX_NB_BUS_SCHEDULE];
  struct tm timeinfo;
  char timeString[6];
  char buf[15];

  // Met l'ESP32 en veille profonde et eteind l'ecran
  // après 1 minutes (30 sec par loop)
  loopCounter++;
  if (loopCounter > 2) {
    enterDeepSleep();
  }

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

  Serial.printf("numBuses = %d\n", numBuses);
  if (numBuses > MAX_NB_BUS_SCHEDULE) {
    Serial.printf("Erreur\n");
    return;
  }

  // Affichage de la ligne et de l'heure de départ des prochains bus
  // Le nombre de bus affiches est limite a NB_BUS_DISPLAYED
  // Seuls les bus dont l'horaire de depart est posterieur a
  // l'heure courante sont affiches
  int nbBusDisplayed = 0;
  int nbBus = 0;
  while ((nbBus <= numBuses) && (nbBusDisplayed < NB_BUS_DISPLAYED)) {
    // N'affiche que les bus dont l'heure de depart est superieur a l'heure actuelle
    if (isMyTimeGreater(nextBuses[nbBus].busTime, timeinfo)) {
      Serial.printf("Ligne %s  %02d:%02d\n",
                    nextBuses[nbBus].busLine,
                    nextBuses[nbBus].busTime.heure,
                    nextBuses[nbBus].busTime.minute);

      snprintf(buf, sizeof(buf), "%s  %02d:%02d",
               nextBuses[nbBus].busLine, nextBuses[nbBus].busTime.heure, nextBuses[nbBus].busTime.minute);
      tft.setTextColor(ST77XX_CYAN);
      tft.setCursor(0, 65 + 30 * nbBusDisplayed);
      tft.print(buf);
      nbBusDisplayed++;
    }
    nbBus++;
  }

  // Réactualise toutes les 30 secondes
  delay(30 * 1000);
}
