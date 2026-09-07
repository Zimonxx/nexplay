# NexPlay 0.2.1

## Zmiany edytora

- Przycinanie V1 i wycinanie środka obejmuje również audio — na timeline, w odsłuchu i w eksportowanym pliku.
- Kosz obok nazwy usuwa ścieżkę z montażu i MP4; `Ctrl+Z` przywraca usunięcie. Checkbox wycisza, zachowując cichy strumień. Oryginał nie jest zmieniany.
- Eksport pokazuje procent i pasek postępu. 100% pojawia się dopiero po poprawnym zapisaniu pliku.
- Dekodowanie obrazu przez NVDEC i kodowanie przez NVENC, bez przenoszenia klatek do RAM. Audio i operacje plikowe nadal używają CPU. Bez cichego przełączania kodowania na CPU.
- Zaktualizowane narzędzia FFmpeg — należy wymienić całą paczkę, nie tylko EXE.

Nagrywarka powtórek z NVENC, osobnymi ścieżkami audio aplikacji i mikrofonu oraz wbudowaną biblioteką i edytorem klipów.

## Pobieranie

- **NexPlay-0.2.1-Setup-windows-x64.exe** — instalator dla bieżącego użytkownika, z opcjonalnym skrótem na pulpicie.
- **NexPlay-0.2.1-windows-x64.zip** — wersja przenośna; rozpakuj całość i uruchom `NexPlay/nexplay.exe`.
- **SHA256SUMS.txt** — sumy kontrolne paczek.
- **NexPlay-0.2.1-FFmpeg-source.zip** — dokładne źródła i instrukcja budowania dołączonego FFmpeg (LGPL).

FFmpeg i FFprobe są dołączone. Nie trzeba ich osobno instalować ani ustawiać PATH. NexPlay ma własną ikonę w pliku EXE, trayu, pasku zadań i instalatorze. Kod i oryginalna grafika: MIT.

## Używanie

Wybierz parametry i źródła audio, a następnie kliknij **Uruchom bufor**. Domyślnie **F8** zapisuje klip i zeruje bufor, a **F9** zatrzymuje bufor. Skróty można zmienić i wyłączyć. Zapis ma cichy wskaźnik postępu w wybranym rogu ekranu. Biblioteka oferuje miniatury z podglądem klatka po klatce, odtwarzacz, przycinanie, wyciszanie i łączenie ścieżek audio.

Zamknięcie okna chowa aplikację do trayu. Przed aktualizacją zakończ NexPlay przez menu ikony i poczekaj na zapis klipów. Aktualizacja zachowuje ustawienia, a odinstalowanie nie usuwa nagrań. Autostart oraz automatyczny bufor są osobnymi ustawieniami — instalator ich sam nie włącza.

## Wymagania i ważne informacje

- Windows 11 x64 oraz NVIDIA z NVENC i aktualnym sterownikiem. Nagrywanie na AMD/Intel nie jest obsługiwane.
- Dostępne rozdzielczości, FPS i bitrate zależą od sprzętu. Windows N wymaga Media Feature Pack.
- Wydanie **nie jest podpisane cyfrowo**; Windows może wyświetlić ostrzeżenie. Pobieraj z tego repozytorium i sprawdzaj SHA256. Nie wyłączaj ochrony systemu.
- Testy automatyczne nie zastępują sprawdzenia na wszystkich konfiguracjach sprzętu.

Instrukcja używania i budowania: [README](https://github.com/Zimonxx/nexplay#readme). Problemy zgłaszaj w [Issues](https://github.com/Zimonxx/nexplay/issues), podając wersję systemu, GPU i kroki odtworzenia.
