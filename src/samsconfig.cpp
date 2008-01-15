/***************************************************************************
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <fstream>

#include "configmake.h"
#include "config.h"

#ifdef USE_UNIXODBC
#include "odbcconn.h"
#include "odbcquery.h"
#endif

#ifdef USE_MYSQL
#include "mysqlconn.h"
#include "mysqlquery.h"
#endif

#include "samsconfig.h"
#include "debug.h"
#include "tools.h"

bool SamsConfig::_loaded = false;
bool SamsConfig::_internal = false;
DBConn::DBEngine SamsConfig::_engine;
map < string, string > SamsConfig::_attributes;

bool SamsConfig::load()
{
  if (_loaded)
    return true;

  if (_internal)
    return true;

  return reload();
}

bool SamsConfig::reload()
{
  if (!readFile())
    return false;

  if (!readDB())
    return false;

  _loaded = true;

  return true;
}

bool SamsConfig::readFile ()
{
  fstream in;
  string line;
  string name;
  string value;
  int signpos;

  string conffile = SYSCONFDIR;
  conffile += "/sams2.conf";

  in.open (conffile.c_str (), ios_base::in);
  if (!in.is_open ())
    {
      ERROR ("Failed to open file " << conffile);
      return false;
    }

  while (in.good ())
    {
      getline (in, line);
      line = StripComments (line);

      if (line.empty ())
        continue;

      signpos = line.find_first_of ('=');
      if (signpos < 0)
        continue;

      name = line.substr (0, signpos);
      value = line.substr (signpos + 1, line.size () - signpos);

      name = TrimSpaces (name);
      value = TrimSpaces (value);
      DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << name << "=" << value);
      setString (name, value);
    }

  in.close ();

  map < string, string >::iterator it = _attributes.find (defDBENGINE);
  if (it == _attributes.end ())
    {
      ERROR ("Unspecified DB engine. Check " << defDBENGINE << " in config file.");
      return false;
    }

  string dbengine = (*it).second;

  if (dbengine == "MySQL")
    {
      #ifndef USE_MYSQL
      ERROR ("MySQL engine is not enabled. Reconfigure package to enable it or change engine.");
      return false;
      #else
      DEBUG (DEBUG_DB, "[" << __FUNCTION__ << "] " << "using MySQL engine.");
      _engine = DBConn::DB_MYSQL;
      #endif
    }
  else if (dbengine == "unixODBC")
    {
      #ifndef USE_UNIXODBC
      ERROR ("unixODBC engine is not enabled. Reconfigure package to enable it or change engine.");
      return false;
      #else
      DEBUG (DEBUG_DB, "[" << __FUNCTION__ << "] " << "using unixODBC engine.");
      _engine = DBConn::DB_UODBC;
      #endif
    }
  else
    {
      ERROR ("Unsupported DB engine " << dbengine);
      return false;
    }

  return true;
}

bool SamsConfig::readDB ()
{
  int err;

  _internal = true;

  int proxyid = getInt (defPROXYID, err);

  if (err != ERR_OK)
    {
      ERROR ("No proxyid defined. Check " << defPROXYID << " in config file.");
      _internal = false;
      return false;
    }

  DBConn::DBEngine engine = SamsConfig::getEngine();

  DBConn *conn;
  DBQuery *query = NULL;

  if (engine == DBConn::DB_UODBC)
    {
      #ifdef USE_UNIXODBC
      conn = new ODBCConn();
      query = new ODBCQuery((ODBCConn*)conn);
      #endif
    }
  else if (engine == DBConn::DB_MYSQL)
    {
      #ifdef USE_MYSQL
      conn = new MYSQLConn();
      query = new MYSQLQuery((MYSQLConn*)conn);
      #endif
    }
  else
    {
      _internal = false;
      return false;
    }

  if (!conn->connect ())
    {
      delete query;
      delete conn;
      _internal = false;
      return false;
    }

  basic_stringstream < char >s;

  int s_sleep;
  int s_parser_time;

  s << "select s_sleep, s_parser_time from proxy where s_proxy_id=" << proxyid;

  if (!query->bindCol (1, DBQuery::T_LONG, &s_sleep, 0))
    {
      delete query;
      delete conn;
      _internal = false;
      return false;
    }
  if (!query->bindCol (2, DBQuery::T_LONG, &s_parser_time, 0))
    {
      delete query;
      delete conn;
      _internal = false;
      return false;
    }

  if (!query->sendQueryDirect (s.str ()))
    {
      delete query;
      delete conn;
      _internal = false;
      return false;
    }

  if (!query->fetch ())
    {
      WARNING ("No settings for proxy " << proxyid << ". Somethig wrong?");
      delete query;
      delete conn;
      _internal = false;
      return false;
    }

  setInt (defSLEEPTIME, s_sleep);
  setInt (defDAEMONSTEP, s_parser_time);

  delete query;
  delete conn;
  _internal = false;

  return true;
}

string SamsConfig::getString (const string & attrname, int &err)
{
  if (!_loaded)
    load();

  map < string, string >::iterator it = _attributes.find (attrname);
  if (it == _attributes.end ())
    {
      err = ATTR_NOT_FOUND;
      DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << " not found");
      return "";
    }
  DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << "=" << (*it).second);
  err = ERR_OK;
  return (*it).second;
}

int SamsConfig::getInt (const string & attrname, int &err)
{
  int res;
  string val;

  val = getString (attrname, err);
  if (err == ATTR_NOT_FOUND)
    return 0;
  if (sscanf (val.c_str (), "%d", &res) != 1)
    {
      err = ATTR_NOT_PARSED;
      DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << " not parsed");
      return 0;
    }
  return res;
}

double SamsConfig::getDouble (const string & attrname, int &err)
{
  double res;
  string val;

  val = getString (attrname, err);
  if (err == ATTR_NOT_FOUND)
    return 0;
  if (sscanf (val.c_str (), "%lf", &res) != 1)
    {
      err = ATTR_NOT_PARSED;
      DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << " not parsed");
      return 0;
    }
  return res;
}

bool SamsConfig::getBool (const string & attrname, int &err)
{
  int res;
  string val;

  val = getString (attrname, err);
  if (err == ATTR_NOT_FOUND)
    return 0;
  if (sscanf (val.c_str (), "%d", &res) != 1)
    {
      err = ATTR_NOT_PARSED;
      DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << " not parsed");
      return false;
    }
  if (res == 0)
    return false;
  return true;
}

void SamsConfig::setString (const string & attrname, const string & value)
{
  DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << "=" << value);
  _attributes[attrname] = value;
}

void SamsConfig::setInt (const string & attrname, const int &value)
{
  char buf[64];
  sprintf (&buf[0], "%d", value);
  DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << "=" << buf);
  _attributes[attrname] = buf;
}

void SamsConfig::setDouble (const string & attrname, const double &value)
{
  char buf[64];
  sprintf (&buf[0], "%lf", value);
  DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << "=" << buf);
  _attributes[attrname] = buf;
}

void SamsConfig::setBool (const string attrname, const bool value)
{
  char buf[64];
  sprintf (&buf[0], "%d", (value == true) ? 1 : 0);
  DEBUG (DEBUG9, "[" << __FUNCTION__ << "] " << attrname << "=" << buf);
  _attributes[attrname] = buf;
}

DBConn::DBEngine SamsConfig::getEngine()
{
  if (!_loaded)
    load();

  return _engine;
}
