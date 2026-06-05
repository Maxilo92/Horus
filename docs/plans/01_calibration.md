# Feature: Kalibrierung & Skalierung (Hochpräzision)

## 1. Zielsetzung
Präzise Transformation von 2D-Bildkoordinaten in physikalische Einheiten unter Berücksichtigung von Kameraperspektive und Linsenverzerrung.

## 2. Technische Anforderungen
- **Multi-Point Calibration**: Unterstützung für 2-Punkt (Skalierung) und 4-Punkt (Perspektivkorrektur/Homographie) Kalibrierung.
- **Lens Distortion Compensation**: Option zur Einbindung von Kameraparametern (Intrinsics), um Kissen- oder Tonnenverzeichnungen zu korrigieren.
- **Einheiten-Management**: Unterstützung für metrische (mm, m) und imperiale Einheiten mit konfigurierbarer Präzision (Nachkommastellen).
- **Validierungs-Koeffizient**: Berechnung eines Fehlerschätzwerts (Reprojection Error) zur Qualitätskontrolle der Kalibrierung.

## 3. UI-Integration
- Interaktiver Kalibrierungs-Assistent (Step-by-Step).
- Visualisierung des kalibrierten Gitters (Grid Overlay) zur optischen Kontrolle.
- Anzeige des aktuellen Vertrauensintervalls der Messungen.

## 4. Validierung
- Automatisierter Test gegen statische Referenzmuster.
- Messabweichungs-Protokollierung bei verschiedenen Objektdistanzen.
