#include "character.hpp"

#include <glog/logging.h>

namespace pxd
{

Character::Character (Database& d, const std::string& o, const std::string& n)
  : db(d), id(db.GetNextId ()), owner(o), name(n), dirty(true)
{
  VLOG (1)
      << "Created new character with ID " << id << ": "
      << "owner=" << owner << ", name=" << name;
}

Character::Character (Database::Result& res)
  : db(res.GetDatabase ()), dirty(false)
{
  CHECK_EQ (res.GetName (), "characters");
  id = res.Get<int> ("id");
  owner = res.Get<std::string> ("owner");
  name = res.Get<std::string> ("name");
  pos = HexCoord (res.Get<int> ("x"), res.Get<int> ("y"));
  res.GetProto ("proto", data);

  VLOG (1) << "Fetched character with ID " << id << " from database result";
}

Character::~Character ()
{
  CHECK_GT (id, 0);
  CHECK_NE (name, "");

  if (!dirty)
    {
      VLOG (1) << "Character " << id << " is not dirty, no update";
      return;
    }

  VLOG (1) << "Character " << id << " has been modified, updating DB";
  auto stmt = db.Prepare (R"(
    INSERT OR REPLACE INTO `characters`
      (`id`, `owner`, `name`, `x`, `y`, `ismoving`, `proto`) VALUES
      (?1, ?2, ?3, ?4, ?5, ?6, ?7)
  )");

  stmt.Bind<int> (1, id);
  stmt.Bind (2, owner);
  stmt.Bind (3, name);
  stmt.Bind<int> (4, pos.GetX ());
  stmt.Bind<int> (5, pos.GetY ());
  stmt.Bind (6, data.has_movement ());
  stmt.BindProto (7, data);

  stmt.Execute ();
}

Database::Result
CharacterTable::GetAll ()
{
  auto stmt = db.Prepare ("SELECT * FROM `characters` ORDER BY `id`");
  return stmt.Query ("characters");
}

Database::Result
CharacterTable::GetForOwner (const std::string& owner)
{
  auto stmt = db.Prepare (R"(
    SELECT * FROM `characters` WHERE `owner` = ?1 ORDER BY `id`
  )");
  stmt.Bind (1, owner);
  return stmt.Query ("characters");
}

bool
CharacterTable::IsValidName (const std::string& name)
{
  if (name.empty ())
    return false;

  auto stmt = db.Prepare (R"(
    SELECT COUNT(*) AS `cnt` FROM `characters` WHERE `name` = ?1
  )");
  stmt.Bind (1, name);

  auto res = stmt.Query ();
  CHECK (res.Step ());
  const int cnt = res.Get<int> ("cnt");
  CHECK (!res.Step ());

  VLOG (1) << "Name " << name << " is used " << cnt << " times";
  return cnt == 0;
}

} // namespace pxd