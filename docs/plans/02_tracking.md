# Feature: Persistentes Multi-Objekt-Tracking

## 1. Zielsetzung
Zuweisung einer eindeutigen, dauerhaften Kennung (UUID) an jedes detektierte Objekt, um Bewegungsabläufe über die Zeit analysieren zu können.

## 2. Technische Anforderungen
- **Tracking-Algorithmus**: Einsatz von Centroid Tracking oder Kalman-Filtern zur Vorhersage der Objektposition.
- **ID-Management**: Verwaltung eines ID-Pools; Re-Identifizierung von Objekten nach kurzem Signalverlust.
- **Datenstruktur**: `std::map<int, ObjectHistory>` zur Speicherung der Pfade.

## 3. UI-Integration
- Anzeige der ID direkt über der Bounding-Box im HUD.
- Zeichnen des zurückgelegten Pfads (Breadcrumbs) für aktive IDs.

## 4. Validierung
- Test mit sich kreuzenden Objekten; Prüfung, ob IDs erhalten bleiben.
