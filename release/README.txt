NexPlay — Replay Studio
======================

Wymagania: Windows 11 x64, karta NVIDIA z NVENC i aktualny sterownik NVIDIA.
AMD i Intel nie są obecnie obsługiwane do nagrywania. Maksymalna rozdzielczość,
FPS i bitrate zależą od sprzętu. FFmpeg jest dołączony; nie trzeba ustawiać PATH.

Instalator instaluje program dla bieżącego użytkownika, bez wymagania praw
administratora. Skrót na pulpicie jest opcjonalny. Wersję ZIP rozpakuj w całości,
zachowując katalog tools obok nexplay.exe. Nie uruchamiaj programu wewnątrz ZIP.

Uruchom NexPlay, wybierz parametry i źródła dźwięku, kliknij Uruchom bufor.
F8 zapisuje klip i zeruje bufor; F9 zatrzymuje bufor. Skróty możesz zmienić
lub wyłączyć. Biblioteka pozwala przeglądać, przycinać i eksportować klipy.
Zamknięcie okna chowa program do trayu. Aby wyjść, wybierz Zakończ w menu ikony.
Przed ręczną instalacją lub odinstalowaniem zakończ program i poczekaj na zapis klipów.

Zakładka Aktualizacje sprawdza nowe stabilne wydania automatycznie i na żądanie.
Nowa wersja wywołuje ciche powiadomienie. Pobierz aktualizację w aplikacji,
zatrzymaj bufor, zakończ eksport i wybierz Zaktualizuj i uruchom ponownie.
Program sprawdzi paczkę i podmieni swoje pliki po zamknięciu, bez instalatora.
Nagrania i ustawienia pozostają bez zmian. Zmiany montażu wcześniej wyeksportuj.
Automatyczne sprawdzanie można wyłączyć w zakładce Aktualizacje.

Autostart i automatyczny bufor włącza się osobno w ustawieniach aplikacji.
Instalator nie włącza ich sam. Aktualizacja zachowuje dotychczasowy wybór.
Odinstalowanie nie usuwa nagrań ani ustawień użytkownika.

Nagrania: %USERPROFILE%\Videos\NexPlay\Clips
Bufor: %TEMP%\NexPlay\ReplayBuffer

To wydanie nie ma podpisu cyfrowego wydawcy. Windows może wyświetlić ostrzeżenie.
Pobieraj wyłącznie z oficjalnych Releases; sumy SHA256 są dostępne obok paczek.
Nie wyłączaj ochrony systemu. Szczegółowa instrukcja: README.md.

Licencja NexPlay: MIT. Zależności: THIRD-PARTY-NOTICES.txt i katalog licenses.
Kod źródłowy, wydania i zgłoszenia: https://github.com/Zimonxx/nexplay
