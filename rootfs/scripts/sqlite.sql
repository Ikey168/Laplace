-- The SQLite gate's script (#256): create, fill, index, and query a
-- database file, inside a transaction, then read the file's size back.
.bail on
.headers off
CREATE TABLE seeds(id INTEGER PRIMARY KEY, word TEXT, weight REAL);
BEGIN;
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 20000)
INSERT INTO seeds(word, weight) SELECT printf('w%05d', (i * 7919) % 20000), (i % 97) / 7.0 FROM n;
COMMIT;
CREATE INDEX seeds_word ON seeds(word);
SELECT 'rows', count(*), printf('%.3f', sum(weight)) FROM seeds;
SELECT 'first', word FROM seeds ORDER BY word LIMIT 1;
SELECT 'heaviest', count(*) FROM seeds WHERE weight > 13;
UPDATE seeds SET weight = weight * 2 WHERE id % 3 = 0;
DELETE FROM seeds WHERE id % 5 = 0;
SELECT 'after', count(*), printf('%.3f', sum(weight)) FROM seeds;
-- Inputs the recording journals, kept in the file: a random number (SQLite
-- seeds its generator from /dev/urandom) and the clock, to the millisecond.
CREATE TABLE inputs(r INTEGER, t REAL);
INSERT INTO inputs VALUES (random(), julianday('now'));
SELECT 'random', typeof(r), t > 2400000 FROM inputs;
PRAGMA integrity_check;
SELECT 'pages', page_count FROM pragma_page_count();
.quit
