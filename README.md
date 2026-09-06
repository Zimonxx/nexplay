# NexPlay

[![Build](https://github.com/Zimonxx/nexplay/actions/workflows/windows-ci.yml/badge.svg)](https://github.com/Zimonxx/nexplay/actions/workflows/windows-ci.yml)
[![Release](https://github.com/Zimonxx/nexplay/actions/workflows/release.yml/badge.svg)](https://github.com/Zimonxx/nexplay/actions/workflows/release.yml)

NexPlay to lekka nagrywarka powtórek dla Windows, korzystająca ze sprzętowego kodowania NVIDIA NVENC. Program stale przechowuje ostatni fragment rozgrywki w buforze tymczasowym, a po użyciu skrótu zapisuje go jako klip i od razu rozpoczyna nowy bufor.

## Funkcje

- sprzętowe kodowanie H.264 przez NVIDIA NVENC,
- przechwytywanie całego monitora ustawionego jako główny,
- natywna rozdzielczość monitora, również do 8K,
- konfigurowalny FPS, bitrate i długość bufora od 10 sekund do 20 minut,
- bufor powtórek przechowywany w katalogu tymczasowym systemu,
- zapis klipu i wyzerowanie bufora jednym globalnym skrótem,
- oddzielna ścieżka audio dla każdej aktywnej aplikacji oraz mikrofonu,
- łączenie wybranych aplikacji w jedną nazwaną ścieżkę audio,
- możliwość wykluczania wybranych aplikacji i mikrofonu z nagrania,
- globalne skróty działające również wtedy, gdy NexPlay jest aktywnym oknem,
- zmiana skrótów i koloru akcentu w ustawieniach,
- automatyczne uruchamianie z Windows i automatyczny start bufora,
- biblioteka klipów w formie siatki z miniaturami przechowywanymi w pamięci,
- podgląd klatka po klatce podczas przesuwania kursora po miniaturze,
- wbudowany odtwarzacz z przewijaniem, pełnym ekranem i skrótami klawiaturowymi,
- kliknięcie obrazu zatrzymuje lub wznawia odtwarzanie,
- menu kontekstowe klipów, zmiana nazwy i otwieranie lokalizacji pliku,
- prosty edytor do przycinania początku i końca klipu,
- usuwanie zaznaczonego fragmentu ze środka i składanie pozostałości w jeden klip,
- wspólny timeline V1/A1–An z jedną skalą czasu, wyciszaniem i zakresem słyszalności ścieżek,
- opcja połączenia końcowej edycji w jedną ścieżkę audio,
- ciemny interfejs studia z boczną nawigacją, panelem eksportu i konfigurowalnym akcentem,
- responsywne okno: siatka klipów i timeline dopasowują się do dostępnego miejsca, bez rozciągania ikon i checkboxów,
- maksymalizacja do obszaru roboczego Windows i przywracanie rozmiaru okna.

## Wymagania

- Windows 11 x64,
- karta NVIDIA obsługująca NVENC i aktualny sterownik NVIDIA,
- `ffmpeg.exe` oraz `ffprobe.exe` dostępne w zmiennej środowiskowej `PATH`,
- mikrofon i urządzenia audio widoczne w ustawieniach dźwięku Windows.

NexPlay korzysta z NVENC do kodowania obrazu. Zakres obsługiwanych rozdzielczości, liczby klatek i bitrate'u zależy od możliwości konkretnej karty graficznej, sterownika i monitora.

## Instalacja i uruchomienie

1. Pobierz najnowsze archiwum z sekcji [Releases](https://github.com/Zimonxx/nexplay/releases).
2. Rozpakuj je do wybranego katalogu.
3. Upewnij się, że FFmpeg i FFprobe są zainstalowane i dostępne w `PATH`.
4. Uruchom `nexplay.exe`.
5. Ustaw długość bufora, FPS i bitrate oraz wybierz źródła audio.
6. Kliknij **Uruchom bufor**.

Domyślne skróty globalne:

- `F8` — zapisz ostatni fragment i wyzeruj bufor,
- `F9` — zatrzymaj nagrywanie.

Skróty można zmienić w ustawieniach. NexPlay może działać w tle w zasobniku systemowym. Zapisane klipy trafiają domyślnie do:

```text
%USERPROFILE%\Videos\NexPlay\Clips
```

Roboczy bufor nagrania jest przechowywany w:

```text
%TEMP%\NexPlay\ReplayBuffer
```

## Biblioteka i edycja klipów

Zakładka **Biblioteka** pokazuje nagrania w siatce, której liczba kolumn zależy od szerokości okna. Przesunięcie kursora po miniaturze pozwala podejrzeć dokładną klatkę odpowiadającą pozycji kursora. Kliknięcie otwiera klip w odtwarzaczu, a prawy przycisk myszy udostępnia najważniejsze operacje na pliku.

W odtwarzaczu można przewijać nagranie bez jego modyfikowania, przełączyć obraz na pełny ekran i sterować odtwarzaniem klawiaturą. Podgląd znajduje się nad wspólnym timeline obrazu i dźwięku, a po prawej jest panel eksportu.

- Kliknij obraz lub naciśnij `Spację`, aby zatrzymać albo wznowić film.
- Kliknij podziałkę czasu, aby przewinąć bez zmiany zakresu klipu.
- Przeciągnij uchwyty ścieżki **V1**, aby ustawić początek i koniec.
- W panelu eksportu przełącz **Przycinanie brzegów** na **Wycinanie fragmentu**, aby usunąć zaznaczony środek i połączyć pozostałości w jeden plik.
- Checkboxy **A1–An** wyciszają ścieżki w podglądzie i wykluczają je z eksportu; uchwyty na ścieżkach określają ich słyszalny zakres również podczas odsłuchu. Wyciszenia pozostają aktywne na pełnym ekranie. Przewiń listę kółkiem myszy, aby zobaczyć kolejne ścieżki.
- Wpisz nazwę nowego pliku, opcjonalnie włącz **Jedna ścieżka audio** i wybierz **Eksportuj klip**. Oryginał pozostaje bez zmian.

## Budowanie ze źródeł

Do zbudowania projektu potrzebne są:

- Visual Studio 2022 lub Build Tools 2022 z pakietem **Desktop development with C++**,
- CMake 3.24 lub nowszy,
- Ninja albo generator Visual Studio,
- Git z obsługą submodułów.

Pobierz repozytorium razem z submodułami:

```powershell
git clone --recurse-submodules https://github.com/Zimonxx/nexplay.git
cd nexplay
```

Jeżeli repozytorium zostało pobrane bez submodułów:

```powershell
git submodule update --init --recursive
```

Konfiguracja, kompilacja i testy wersji Release:

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
ctest --test-dir out/build/windows-x64-release -C Release --output-on-failure
```

Gotowy program znajduje się w:

```text
out\build\windows-x64-release\Release\nexplay.exe
```

Konfiguracja Debug:

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
ctest --test-dir out/build/windows-x64-debug -C Debug --output-on-failure
```

### Kontrola układu interfejsu

Dodatkowy cel deweloperski sprawdza geometrię paneli, proporcje miniatur i zgodność obszarów kliknięcia. Renderuje 12 podglądów czterech zakładek w trzech rozmiarach oraz podglądy menu klipów i tworzenia grupy audio, używając tego samego renderera co aplikacja:

```powershell
cmake --build --preset windows-x64-release --target nexplay_ui_preview
./out/build/windows-x64-release/Release/nexplay_ui_preview.exe ./out/ui-preview
```

Podglądy zawierają dane testowe. Narzędzie nie otwiera okna, nie nagrywa ekranu i nie instaluje skrótów. Nie zastępuje ręcznej kontroli odtwarzania ani systemowych animacji okna.

## Wydania

Tag w formacie `vX.Y.Z` uruchamia automatyczne budowanie paczki i publikację wydania na GitHubie:

```powershell
git tag v0.2.0
git push origin v0.2.0
```

Przepływ wydania można również uruchomić ręcznie w zakładce **Actions**.
