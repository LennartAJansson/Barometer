import os
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

# Läs .env från projektroten (inte aktuell arbetskatalog) om den finns
env_path = os.path.join(env.subst("$PROJECT_DIR"), ".env")
if os.path.exists(env_path):
    with open(env_path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            # Hoppa över tomma rader och kommentarer
            if not line or line.startswith("#") or "=" not in line:
                continue

            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip('"').strip("'")

            # Sätt variabeln i operativsystemets miljö för denna process
            os.environ[key] = value

            # Sätt variabeln i PlatformIOs interna byggmiljö (sysenv)
            env["ENV"][key] = value
            if key in ("WIFI_SSID", "WIFI_PASS"):
                env.Append(CPPDEFINES=[(key, '\\"' + value + '\\"')])
