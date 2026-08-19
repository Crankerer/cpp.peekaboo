### Modernes, lesbares und wartbares C++ verwenden

Generiere ausschließlich modernen, idiomatischen C++-Code. Verwende nach Möglichkeit **C++20 oder neuer** und nutze moderne Sprachfeatures und die Standardbibliothek sinnvoll.

Der Code soll nicht nur funktionieren, sondern **einfach, kurz, übersichtlich, gut lesbar und langfristig wartbar** sein. Bevorzuge die einfachste Lösung, die das Problem sauber und robust löst.

### Grundprinzipien

- Verwende bevorzugt **C++20 oder neuer**.
- Schreibe **möglichst kurzen und prägnanten Code**, ohne die Lesbarkeit zu beeinträchtigen.
- Vermeide unnötige Abstraktionen, Wrapper, Klassen, Helper-Funktionen und Design Patterns.
- **Keine Overengineering-Lösungen**: Nicht jede Kleinigkeit muss abstrahiert werden.
- Bevorzuge einfache, direkte Lösungen gegenüber unnötig komplexen Architekturen.
- Code soll auf den ersten Blick verständlich sein.
- Verwende aussagekräftige und kurze Namen für Variablen, Funktionen und Typen.
- Vermeide unnötige Kommentare. Kommentare sollen nur erklären, **warum** etwas gemacht wird, nicht offensichtliches **was**.
- Vermeide unnötige Verschachtelungen und lange Funktionen.
- Bevorzuge Early Returns und Guard Clauses, wenn sie den Kontrollfluss vereinfachen.
- Halte Funktionen möglichst klein und fokussiert.
- Vermeide Code-Duplikation, aber erstelle dafür keine unnötigen Abstraktionen.
- Bevorzuge **Composition statt unnötiger Vererbung**.

### Wenige Dateien und übersichtliche Struktur

Halte die Anzahl der Dateien **so gering wie sinnvoll möglich**.

- Erstelle nicht für jede Klasse, Struktur oder kleine Funktion eine eigene Datei.
- Kleine, eng zusammengehörige Komponenten dürfen sich in derselben Header-/Source-Datei befinden.
- Teile Code nur dann auf mehrere Dateien auf, wenn dies die Übersichtlichkeit oder Wartbarkeit tatsächlich verbessert.
- Vermeide unnötige Verzeichnis- und Modulstrukturen.
- Die Projektstruktur soll auf einen Blick verständlich sein.
- Bevorzuge eine **flache und übersichtliche Struktur** gegenüber einer tief verschachtelten Architektur.
- Vermeide Boilerplate und Dateien, die nur wenige Zeilen enthalten, sofern sie keinen klaren Zweck erfüllen.

### Modernes C++ statt C-Style

Vermeide alte C-Style-Patterns, sofern eine moderne C++-Alternative existiert:

- Keine manuellen `new`/`delete`-Aufrufe → RAII und Smart Pointer verwenden.
- Keine C-Style-Arrays → `std::array`, `std::vector` oder passende Container verwenden.
- Keine C-Style-Casts → passende C++-Casts oder bessere Typen verwenden.
- `nullptr` statt `NULL` oder `0`.
- `enum class` statt klassischer Enums.
- `std::string` / `std::string_view` statt C-Strings.
- Moderne STL-Container und Algorithmen verwenden.
- `std::optional`, `std::variant` und `std::expected` einsetzen, wenn sie das Design vereinfachen.
- `constexpr`, `consteval`, `const`, `noexcept` und `[[nodiscard]]` sinnvoll einsetzen.
- Concepts, Ranges und Structured Bindings verwenden, wenn sie den Code tatsächlich klarer machen.
- `auto` verwenden, wenn dadurch der Code besser lesbar wird.
- Keine unnötigen Makros.
- Keine unnötige Verwendung der C-Standardbibliothek, wenn eine bessere C++-Alternative existiert.
- C-APIs, falls notwendig, möglichst an einer klar abgegrenzten Stelle kapseln.

### Lesbarkeit vor Cleverness

Bevorzuge immer:

**klarer Code > cleverer Code**  
**einfacher Code > abstrakter Code**  
**weniger Code > mehr Boilerplate**  
**wenige Dateien > unnötige Aufteilung**  
**moderne C++-Features > alte C-Patterns**

Vermeide insbesondere extrem kompakten oder „cleveren“ Code, der zwar kurz ist, aber schwer verständlich wird. Kürze darf niemals auf Kosten der Lesbarkeit gehen.

Wenn eine Lösung beispielsweise mit 10 klaren Zeilen statt mit 3 kryptischen Zeilen umgesetzt werden kann, ist die klarere Lösung zu bevorzugen.

### Speicher und Ressourcen

- Verwende **RAII** konsequent.
- Bevorzuge `std::unique_ptr` gegenüber `std::shared_ptr`, wenn Shared Ownership nicht tatsächlich benötigt wird.
- Vermeide rohe Pointer, sofern sie nicht ausdrücklich als Non-Owning Pointer benötigt werden.
- Vermeide unnötige Kopien.
- Nutze Move-Semantik sinnvoll, aber nicht zwanghaft.
- Ressourcen wie Dateien, Locks, Sockets oder Handles sollen automatisch und sicher verwaltet werden.

### Fehlerbehandlung

- Verwende eine zur Situation passende, moderne Fehlerbehandlung.
- Bevorzuge `std::optional` für „Wert vorhanden oder nicht“.
- Bevorzuge `std::expected` für erwartbare Fehler, sofern verfügbar.
- Verwende Exceptions dort, wo sie sinnvoll sind.
- Vermeide Fehlercodes und manuelle Fehlerbehandlung im C-Stil, wenn eine bessere C++-Lösung möglich ist.

### Standardbibliothek bevorzugen

Bevor eigene Lösungen geschrieben werden, prüfen, ob die C++-Standardbibliothek bereits eine passende Lösung bietet.

Bevorzuge beispielsweise:

- `std::vector`
- `std::array`
- `std::string`
- `std::string_view`
- `std::span`
- `std::optional`
- `std::variant`
- `std::expected`
- `std::filesystem`
- `std::chrono`
- `std::ranges`
- STL-Algorithmen
- Smart Pointer
- Lambdas

### Architektur

Die Architektur soll **so einfach wie möglich und so strukturiert wie nötig** sein.

Vermeide:

- unnötige Design Patterns
- unnötige Interfaces
- unnötige Abstraktionsschichten
- übermäßige Template-Komplexität
- Deep Inheritance Hierarchies
- unnötige Dependency Injection
- Klassen ohne echten Mehrwert
- unnötige Wrapper um STL-Typen
- riesige Framework-artige Strukturen für kleine Probleme

Führe eine Abstraktion nur dann ein, wenn sie einen **konkreten Vorteil** für Lesbarkeit, Wiederverwendung, Testbarkeit oder Wartbarkeit bringt.

### Ziel

Der fertige Code soll aussehen, als wäre er von einem erfahrenen C++-Entwickler geschrieben worden:

**modern, kompakt, klar, typsicher, robust, wartbar und ohne unnötigen Ballast.**

Bei mehreren möglichen Lösungen ist grundsätzlich die Variante zu bevorzugen, die mit **weniger Code, weniger Dateien und weniger Abstraktionen** eine saubere und verständliche Lösung erreicht – solange Sicherheit, Wartbarkeit und Lesbarkeit erhalten bleiben.