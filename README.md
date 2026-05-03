# 1. Установка Docker (если ещё не установлен)

# 2. Клонирование проекта
git clone <repo-url>
cd Android-backend

# 3. Создание файла инициализации БД
cat > db/init/1-init.sql << 'EOF'
CREATE TABLE IF NOT EXISTS network_logs (
    id SERIAL PRIMARY KEY,
    latitude DOUBLE PRECISION NOT NULL,
    longitude DOUBLE PRECISION NOT NULL,
    altitude DOUBLE PRECISION,
    accuracy DOUBLE PRECISION,
    network_type VARCHAR(10),
    rsrp DOUBLE PRECISION,
    timestamp TIMESTAMPTZ DEFAULT NOW()
);
EOF

# 5. Загрузка исторических данных (если есть log.json)
python3 generate_init_sql.py log.json db/init/1-init.sql

# 6. Разрешить Docker доступ к экрану
xhost +local:docker

# 7. Запуск приложения
docker compose up --build

# 8. Остановка приложения
docker compose down

# 9. Остановка с удалением данных базы
docker compose down -v