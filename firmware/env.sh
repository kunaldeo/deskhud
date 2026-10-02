# source this: sets up ESP-IDF v5.5.5 for this project (IDF 5.5 needs Python <= 3.13)
export IDF_PATH="$HOME/esp/esp-idf-v5.5.5"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.12_env"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
. "$IDF_PATH/export.sh" >/dev/null
