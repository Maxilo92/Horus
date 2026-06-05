# Feature: Vektoranalyse & Geschwindigkeitsmessung

## 1. Zielsetzung
Berechnung und Visualisierung der Bewegungsdynamik von Objekten in Echtzeit.

## 2. Technische Anforderungen
- **Vektorberechnung**: Differenzbildung der Positionen über ein Zeitfenster (z.B. gleitender Durchschnitt über 5 Frames).
- **Einheiten-Konvertierung**: Nutzung der Kalibrierungsdaten zur Ausgabe in m/s oder km/h.
- **Richtung**: Berechnung des Winkels der Bewegungstrajektorie.

## 3. UI-Integration
- Anzeige eines Richtungsvektors (Pfeil) an der Bounding-Box.
- Overlay mit dem aktuellen Geschwindigkeitswert.

## 4. Validierung
- Test mit Objekten mit bekannter Geschwindigkeit (z.B. Förderband oder motorisierter Schlitten).
