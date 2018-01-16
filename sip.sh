###############################################################################
sudo make prefix=/usr/local/sip232/ all include_modules="db_mysql" 
###############################################################################
# this parameter.
DBENGINE=MYSQL

## database port (PostgreSQL=5432 default; MYSQL=3306 default)
DBPORT=3306

## database host
DBHOST=localhost

## database name (for ORACLE this is TNS name)
DBNAME=opensips

# database path used by dbtext, db_berkeley, or sqlite
# DB_PATH="/usr/local/etc/opensips/dbtext"

## database read/write user
DBRWUSER=opensips

## password for database read/write user
DBRWPW="opensipsrw"

## engine type for the MySQL/MariaDB tabels (default InnoDB)
# MYSQL_ENGINE="MyISAM"

## database super user (for ORACLE this is 'scheme-creator' user)
DBROOTUSER="root"
################################################################################
mysql> grant all on *.* to 'opensips'@'%' identified by 'opensipsrw';
Query OK, 0 rows affected (0.09 sec)

mysql> flush privileges;
Query OK, 0 rows affected (0.10 sec)
################################################################################
sudo cp ./opensips-2.3.2/scripts/opensipsdbctl.mysql /usr/local/sip232//lib/opensips/opensipsctl/ -a
sudo cp ./opensips-2.3.2/scripts/mysql /usr/local/sip232//lib/opensips/opensipsctl/ -a
################################################################################
$sudo /usr/local/sip232/sbin/opensipsdbctl create 
MySQL password for root: 
INFO: test server charset
INFO: creating database opensips ...
INFO: Using table engine InnoDB.
INFO: Core OpenSIPS tables successfully created.
Install presence related tables? (y/n): y
INFO: creating presence tables into opensips ...
INFO: Presence tables successfully created.
Install tables for imc cpl siptrace domainpolicy carrierroute userblacklist b2b cachedb_sql registrant call_center fraud_detection emergency? (y/n): y
INFO: creating extra tables into opensips ...
INFO: Extra tables successfully created.
################################################################################
sudo cp modules/db_mysql/db_mysql.so /usr/local/sip232/lib/opensips/modules/ -a
################################################################################
opensipsctl add 10000 123456
opensipsctl add 10001 123456
###############################################################################
sudo /usr/local/sip232/sbin/opensipsctl ul show
###############################################################################

###############################################################################

