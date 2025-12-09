#ifndef LINEMAPPING_H
#define LINEMAPPING_H

#include <Arduino.h>  // pour String

// Identifiant de l'arret
const char* stopPointRef = "STIF:StopPoint:Q:28607:";

// Structure pour stocker le mapping
struct LineMapping {
  String ref;     // LineRef PRIM
  String number;  // numero de la ligne
};

// Tableau de correspondance entre les references PRIM des lignes et le numero des lignes
static const LineMapping lineMappings[] = {
  // Cette table contient la liste des lignes de bus s'arretant a l'arret stop_point_ref
  // et dont vous souhaitez les horaires des prochains passages
  { "STIF:Line::C01215:", "195" },
  { "STIF:Line::C01314:", "388" },
};

// Taille du tableau
static const int lineMappingsSize = sizeof(lineMappings) / sizeof(LineMapping);

// Fonction pour récupérer le numéro public de la ligne de bus à partir de son identifiant LineRef
inline String mapLineRefToNumber(const String& lineRef) {
  for (int i = 0; i < lineMappingsSize; i++) {
    if (lineMappings[i].ref == lineRef) {
      return lineMappings[i].number;
    }
  }
  return "XXX";  // inconnu
}

#endif  // LINEMAPPING_H
