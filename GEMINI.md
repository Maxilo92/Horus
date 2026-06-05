# Project Horus - Military-Grade Entwicklungsmandate

Diese Datei definiert die absolut bindenden Standards für Project Horus. Da diese Applikation als **Military-Grade Instrument** konzipiert ist, müssen alle Implementierungen höchsten Anforderungen an Präzision, Zuverlässigkeit, Sicherheit und Echtzeitfähigkeit genügen.

## 1. Oberste Prinzipien (Core Mandates)
- **Zero-Show Policy**: Jede Zeile Code und jedes UI-Element muss einen direkten funktionalen Zweck erfüllen. Keine Dekoration, kein "Flavor".
- **Deterministisches Verhalten**: Algorithmen müssen unter definierten Bedingungen vorhersehbare Ergebnisse liefern. Vermeidung von Race Conditions und unvorhersehbarem Speicherverhalten.
- **Daten-Integrität**: Jedes Byte an aufgezeichneten Daten muss verifizierbar und gegen Korruption geschützt sein (Prüfsummen, atomares Logging).

## 2. Technische Spezifikationen (Military Standards)
- **Echtzeit-Performance**: Der Main-Loop muss ohne dynamische Speicherallokation (Heap) auskommen, um Jitter zu vermeiden. Nutze statische Buffer oder Memory-Pools.
- **Mathematische Präzision**: Nutze Double-Precision für alle physikalischen Berechnungen. Jede Transformation muss auf numerische Stabilität geprüft sein.
- **Hardened Error Handling**: Fehler dürfen niemals zum Absturz führen. Jedes Modul muss isoliert sein und definierte "Safe-States" einnehmen können.
- **Auditable Code**: Der Code muss so strukturiert sein, dass er jederzeit einer formalen Verifizierung oder einem Sicherheits-Audit standhält (klare Besitzverhältnisse, keine Seiteneffekte).

## 3. Workflow & Planung
- **Source of Truth**: Die Pläne in `docs/plans/` sind keine Vorschläge, sondern Spezifikationen. Abweichungen sind nur nach vorheriger Dokumentation im Plan zulässig.
- **Validierungs-Zwang**: Jede Änderung muss das "Rigorose Validierungs-Protokoll" (`09_validation_protocol.md`) durchlaufen. Ein Feature gilt erst als fertig, wenn die Ground-Truth-Validierung erfolgreich war.
- **Versions-Kontrolle**: Jede Version muss exakt reproduzierbar sein. Abhängigkeiten müssen fixiert (pinned) sein.

## 4. Sicherheits-Anforderungen
- **Memory Safety**: Bevorzugung von RAII und Smart Pointern. Verzicht auf rohe Pointer-Arithmetik, wo immer möglich.
- **Input-Validierung**: Jedes Signal vom Sensor (Kamera) und jede Benutzereingabe muss auf Plausibilität geprüft werden, bevor sie verarbeitet wird.

---
*Hinweis: Project Horus wird wie ein Missions-kritisches System behandelt. Nachlässigkeit in der Implementierung ist ein Verstoß gegen das Projektmandat.*
