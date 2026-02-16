#!/bin/bash

# Directory dello script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/venv"
PYTHON_EXEC="python3"

# 1. Controllo FFmpeg (essenziale per shazamio)
if ! command -v ffmpeg &> /dev/null; then
    echo "Errore: FFmpeg non trovato. Installalo con: sudo apt install ffmpeg"
    exit 1
fi

# 2. Gestione Ambiente Virtuale (Auto-setup)
# Se non esiste la cartella venv, proviamo a crearla
if [ ! -d "$VENV_DIR" ]; then
    # Tenta di creare il venv
    python3 -m venv "$VENV_DIR" &> /dev/null
    if [ $? -ne 0 ]; then
        # Se fallisce (es. manca python3-venv), proviamo a usare python di sistema
        # ma avvisiamo che potrebbe fallire l'installazione dei pacchetti
        PYTHON_EXEC="python3"
    else
        PYTHON_EXEC="$VENV_DIR/bin/python3"
        # Aggiorna pip nel venv per sicurezza
        "$VENV_DIR/bin/pip" install --upgrade pip &> /dev/null
    fi
else
    PYTHON_EXEC="$VENV_DIR/bin/python3"
fi

# 3. Installazione Dipendenze (se necessario)
# Controlla se shazamio è importabile con l'interprete selezionato
if ! "$PYTHON_EXEC" -c "import shazamio" &> /dev/null; then
    echo "Installazione dipendenze in corso (potrebbe richiedere un attimo)..."
    
    if [[ "$PYTHON_EXEC" == *"venv"* ]]; then
        "$VENV_DIR/bin/pip" install shazamio &> /dev/null
    else
        # Tentativo su sistema (con break-system-packages per le nuove distro)
        python3 -m pip install shazamio --break-system-packages &> /dev/null
    fi
    
    # Ricontrolla dopo installazione
    if ! "$PYTHON_EXEC" -c "import shazamio" &> /dev/null; then
        echo "Errore: Impossibile installare 'shazamio'."
        if [[ "$PYTHON_EXEC" == *"venv"* ]]; then
             echo "Verifica la connessione internet."
        else
             echo "Prova a installare manualmente: pip3 install shazamio --break-system-packages"
             echo "Oppure installa il modulo venv: sudo apt install python3-venv"
        fi
        exit 1
    fi
fi

# 4. Esecuzione Script Python
$PYTHON_EXEC - "$1" <<EOF
import sys
import asyncio

async def main():
    try:
        from shazamio import Shazam
        shazam = Shazam()
        out = await shazam.recognize(sys.argv[1])
        
        if 'track' in out:
            track = out['track']
            title = track.get('title', 'Sconosciuto')
            artist = track.get('subtitle', 'Sconosciuto')
            # Formato richiesto dal player C++: "Titolo by Artista"
            print(f"{title} by {artist}")
        else:
            print("Nessun risultato trovato.")
            
    except Exception as e:
        print(f"Errore durante il riconoscimento: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Errore: Nessun file audio specificato.")
        sys.exit(1)
    asyncio.run(main())
EOF