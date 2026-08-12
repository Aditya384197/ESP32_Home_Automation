-- Run once for an existing V2.1 D1 database before deploying the duration-enabled scheduler.
ALTER TABLE schedules ADD COLUMN duration_minutes INTEGER NOT NULL DEFAULT 0;
