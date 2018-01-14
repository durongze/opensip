INSERT INTO version (table_name, table_version) values ('registrant','1');
CREATE TABLE registrant (
    id NUMBER(10) PRIMARY KEY,
    registrar VARCHAR2(128) DEFAULT '',
    proxy VARCHAR2(128) DEFAULT NULL,
    aor VARCHAR2(128) DEFAULT '',
    third_party_registrant VARCHAR2(128) DEFAULT NULL,
    username VARCHAR2(64) DEFAULT NULL,
    password VARCHAR2(64) DEFAULT NULL,
    binding_URI VARCHAR2(128) DEFAULT '',
    binding_params VARCHAR2(64) DEFAULT NULL,
    expiry NUMBER(10) DEFAULT NULL,
    forced_socket VARCHAR2(64) DEFAULT NULL,
    CONSTRAINT registrant_aor_idx  UNIQUE (aor)
);

CREATE OR REPLACE TRIGGER registrant_tr
before insert on registrant FOR EACH ROW
BEGIN
  auto_id(:NEW.id);
END registrant_tr;
/
BEGIN map2users('registrant'); END;
/
