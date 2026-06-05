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

## 5. Versionskontrolle & Source Control (Git)
- **Git-Nutzung**: Die Verwendung von lokalem Git ist für dieses Projekt ZWINGEND. Alle Agenten MÜSSEN Git aktiv für die Versionsverwaltung nutzen.
- **Atomic Commits**: Jede logische Änderung (Bugfix, neues Feature, Refactoring) muss in einem eigenen Commit erfolgen.
- **Commit Messages**: Nutze aussagekräftige, präzise Commit-Messages (z.B. "feat: add ROI validation logic" oder "fix: resolve memory leak in MultiTracker").
- **Staging & Committing**: Agenten sind ausdrücklich dazu aufgefordert, ihre Änderungen zu stagen (`git add`) und nach erfolgreicher Validierung zu committen (`git commit`). 
- **Verbotene Commits**: Committe niemals Build-Artefakte, Logs oder temporäre Dateien (siehe `.gitignore`).

---
*Hinweis: Project Horus wird wie ein Missions-kritisches System behandelt. Nachlässigkeit in der Implementierung ist ein Verstoß gegen das Projektmandat.*
