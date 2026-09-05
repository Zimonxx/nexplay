# NexPlay

[![Windows CI](https://github.com/Zimonxx/nexplay/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Zimonxx/nexplay/actions/workflows/windows-ci.yml)
[![Latest release](https://img.shields.io/github/v/release/Zimonxx/nexplay?display_name=tag)](https://github.com/Zimonxx/nexplay/releases/latest)

Natywna aplikacja dla Windows do nagrywania głównego monitora i zapisywania
powtórek przy użyciu sprzętowego enkodera NVIDIA NVENC.

## Ustalone wymagania

- Windows 11 x64, obecnie testowany na kompilacji 26200.7462.
- Automatyczne nagrywanie monitora oznaczonego w Windows jako główny.
- NVENC z wykrywaniem możliwości GPU w czasie działania; RTX 5060 Ti jest
  urządzeniem testowym, ale nie jest wpisany na stałe jako wymaganie.
- Bufor powtórek domyślnie 10 sekund, konfigurowalny do 20 minut.
- Segmenty bufora znajdują się w `%TEMP%\NexPlay\ReplayBuffer`.
- Osobna ścieżka audio dla każdej aktywnej aplikacji/procesu oraz osobna
  ścieżka domyślnego mikrofonu.
- Docelowo H.264, HEVC i AV1, maksymalnie 8K, zależnie od możliwości GPU.

## Aktualny etap

Pierwszy program diagnostyczny:

- wykrywa rzeczywistą wersję Windows,
- odczytuje rozdzielczość i odświeżanie głównego monitora,
- sprawdza dostępność biblioteki NVENC zainstalowanej ze sterownikiem,
- tworzy prywatny katalog tymczasowego bufora.

Drugi program jest testem właściwej ścieżki obrazu:

- przechwytuje główny monitor przez Desktop Duplication i Direct3D 11,
- obsługuje orientację 0°, 90°, 180° i 270°, obracając klatkę na GPU,
- przekazuje teksturę BGRA bezpośrednio do sprzętowego kodera NVENC,
- nagrywa 10 sekund H.264 w 60 FPS i 25 Mb/s,
- zapisuje wynik jako standardowy plik `.mp4` w katalogu bufora tymczasowego;
  prototyp używa do końcowego opakowania dostępnego lokalnie `ffmpeg.exe`.

`nexplay_nvenc_test` sprawdza sam koder niezależnie od uprawnień do pulpitu,
generując krótki animowany wzór 1280×720.

`nexplay_replay_test` jest pierwszym działającym prototypem bufora powtórek:

- stale zapisuje jednosekundowe segmenty w `%TEMP%`,
- zachowuje domyślnie ostatnie 10 sekund,
- F8 zapisuje klip MP4 do systemowego folderu `Wideo\NexPlay\Clips`,
- po F8 zeruje bufor, więc kolejny klip zaczyna się w chwili poprzedniego zapisu,
- F9 kończy działanie i usuwa tymczasową sesję bufora,
- opcjonalny argument określa 10–1200 sekund bufora.

`nexplay_audio_test` wykrywa aktywne sesje audio na wszystkich aktywnych
wyjściach Windows i przez 10 sekund zapisuje osobny WAV dla każdego procesu
oraz domyślnego mikrofonu. Ścieżki aplikacji mają format PCM 48 kHz, 16 bitów,
stereo; mikrofon zachowuje swoją liczbę kanałów i częstotliwość próbkowania.

`nexplay_av_replay_test` łączy działające elementy w jeden prototyp:

- obraz głównego monitora jest kodowany sprzętowo przez NVENC,
- każda aktywna aplikacja i mikrofon trafiają do osobnej, nazwanej ścieżki AAC
  w tym samym pliku MP4,
- F8 zapisuje klip i natychmiast zeruje zarówno bufor obrazu, jak i audio,
- po F8 lista aktywnych aplikacji audio jest skanowana ponownie,
- F9 kończy program i usuwa niezapisany bufor z katalogu tymczasowego.

`nexplay.exe` jest pierwszą wersją programu z interfejsem Windows:

- ma własny interfejs AMOLED black renderowany przez Direct2D i DirectWrite,
- używa płynnych animacji 60 Hz, subtelnego glow i stanów interaktywnych,
- zawiera panel nagrywania oraz bibliotekę zapisanych klipów,
- biblioteka pokazuje nagrania w dwukolumnowym gridzie z prawdziwymi miniaturami,
- przesuwanie kursora po miniaturze pokazuje dokładną klatkę wynikającą z FPS
  nagrania i pozycji na timeline, podobnie jak w Medal; potrzebne klatki są dekodowane na żądanie
  bezpośrednio z MP4, zapamiętywane w RAM-ie i usuwane przy zamknięciu aplikacji,
- własne menu PPM w stylu AMOLED pozwala otworzyć edytor, odtworzyć plik
  systemowo, pokazać go w folderze, skopiować ścieżkę albo przenieść nagranie
  do Kosza,
- otwiera klipy we wbudowanym odtwarzaczu bez wychodzenia z NexPlay,
- ma edytor nazwy i osi czasu z dwoma uchwytami przycinania,
- pozwala przewijać klip kliknięciem lub przeciągnięciem po osi czasu bez
  zmiany zakresu przycięcia,
- odtwarza podgląd na pełnym ekranie; Escape lub podwójne kliknięcie wraca do
  edytora,
- pełny ekran ma własne kontrolki play/pause, przewijanie, licznik czasu oraz
  informacje o rozdzielczości, FPS i bitrate,
- Spacja przełącza play/pause, strzałki przewijają o 5 sekund, a Home i End
  przechodzą odpowiednio na początek i koniec nagrania,
- eksportuje nowy klip przez NVENC, zachowując wszystkie osobne ścieżki audio,
- pozwala ustawić długość bufora, FPS i bitrate,
- pozwala wybrać natywną rozdzielczość, 1080p, 1440p, 4K albo 8K; skalowanie
  odbywa się na GPU i uwzględnia pionową orientację monitora,
- pokazuje aktywne aplikacje audio i pozwala wykluczyć dowolną z nich,
- pozwala włączyć lub wyłączyć osobną ścieżkę mikrofonu,
- uruchamia i zatrzymuje bufor bez okna konsoli,
- zapisuje klip przyciskiem lub konfigurowalnym skrótem globalnym,
- po zminimalizowaniu lub zamknięciu okna pozostaje w zasobniku systemowym,
- opcjonalnie uruchamia się razem z Windows i może automatycznie włączyć bufor,
- pozwala zmienić globalne skróty zapisu i zatrzymania oraz wykrywa ich konflikty,
- nasłuchuje skrótów pasywnie, więc F8/F9 nadal działają w aktywnej grze lub aplikacji,
- ma pełny picker koloru akcentu z wyborem barwy, nasycenia i jasności oraz
  podglądem wartości HEX na bazie motywu AMOLED Black,
- zapamiętuje czas bufora, FPS, bitrate oraz ustawienie mikrofonu,
- pozwala jednym przyciskiem otworzyć folder zapisanych klipów.

## Budowanie

W Developer PowerShell for Visual Studio 2022:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
./out/build/windows-x64-debug/Debug/nexplay_diagnostics.exe
./out/build/windows-x64-debug/Debug/nexplay_capture_test.exe
./out/build/windows-x64-debug/Debug/nexplay_nvenc_test.exe
./out/build/windows-x64-debug/Debug/nexplay_replay_test.exe
./out/build/windows-x64-debug/Debug/nexplay_audio_test.exe
./out/build/windows-x64-debug/Debug/nexplay_av_replay_test.exe
```

Wersję Release można zbudować poleceniami:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --test-dir out/build/windows-x64-release -C Release --output-on-failure
```

## GitHub i wydania

Repozytorium: [github.com/Zimonxx/nexplay](https://github.com/Zimonxx/nexplay)

Każdy push i pull request do gałęzi `main` uruchamia kompilację oraz testy na
Windows. Wysłanie tagu w formacie `v*`, na przykład `v0.1.0`, dodatkowo tworzy
archiwum `NexPlay-<wersja>-windows-x64.zip` i publikuje je jako GitHub Release.

Gotowe wersje programu są dostępne na stronie
[Releases](https://github.com/Zimonxx/nexplay/releases).

Przykład bufora 20-minutowego:

```powershell
./out/build/windows-x64-debug/Debug/nexplay_replay_test.exe 1200
./out/build/windows-x64-debug/Debug/nexplay_av_replay_test.exe 1200
```

## Plan rozwoju

1. Dodać konfigurację wykluczania aplikacji i mikrofonu.
2. Dodać wybór kodeka, bitrate'u, FPS i długości bufora.
3. Zbudować interfejs działający w zasobniku systemowym.
4. Dodać bibliotekę klipów i prosty edytor przycinania.
5. Dodać instalator i automatyczną diagnostykę zgodności GPU.
