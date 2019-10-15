#!/usr/bin/env python

#   GSP for the Taurion blockchain game
#   Copyright (C) 2019  Autonomous Worlds Ltd
#
#   This program is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""
Tests mining of resources by characters.
"""

from pxtest import PXTest, offsetCoord


class MiningTest (PXTest):

  def isMining (self, charName):
    return self.getCharacters ()[charName].data["mining"]["active"]

  def run (self):
    self.collectPremine ()

    self.pos = [{"x": 10, "y": 100}]
    self.pos.append (offsetCoord (self.pos[0], {"x": 1, "y": 0}, False))
    self.assertEqual (self.getRegionAt (self.pos[0]).getId (),
                      self.getRegionAt (self.pos[1]).getId ())

    self.mainLogger.info ("Prospecting a test region...")
    self.initAccount ("domob", "r")
    self.createCharacters ("domob", 2)
    self.generate (1)
    self.moveCharactersTo ({
      "domob": self.pos[0],
      "domob 2": self.pos[1],
    })
    self.getCharacters ()["domob"].sendMove ({"prospect": {}})
    self.generate (15)

    typ, self.amount = self.getRegionAt (self.pos[0]).getResource ()
    self.log.info ("Found %d of %s at the region" % (self.amount, typ))
    self.reorgBlock = self.rpc.xaya.getbestblockhash ()

    self.mainLogger.info ("Starting to mine...")
    self.getCharacters ()["domob"].sendMove ({"mine": {}})
    self.generate (1)
    self.assertEqual (self.isMining ("domob"), True)
    self.assertEqual (self.isMining ("domob 2"), False)

    self.mainLogger.info ("Movement stops mining...")
    self.getCharacters ()["domob"].sendMove ({"wp": []})
    self.generate (1)
    self.assertEqual (self.isMining ("domob"), False)
    self.assertEqual (self.isMining ("domob 2"), False)

    self.mainLogger.info ("Mining with two characters...")
    for c in self.getCharacters ().values ():
      c.sendMove ({"mine": {}})
    self.generate (1)
    self.assertEqual (self.isMining ("domob"), True)
    self.assertEqual (self.isMining ("domob 2"), True)

    self.mainLogger.info ("Using up all resources...")
    while True:
      _, remaining = self.getRegionAt (self.pos[0]).getResource ()
      if remaining == 0:
        break
      for c in self.getCharacters ().values ():
        c.sendMove ({
          "drop": {"f": {typ: 1000}},
          "mine": {},
        })
      self.generate (50)
    for c in self.getCharacters ().values ():
      c.sendMove ({
        "drop": {"f": {typ: 1000}},
      })
    self.generate (1)
    self.assertEqual (self.isMining ("domob"), False)
    self.assertEqual (self.isMining ("domob 2"), False)

    total = 0
    for loot in self.getRpc ("getgroundloot"):
      inv = loot["inventory"]["fungible"]
      self.assertEqual (len (inv), 1)
      total += inv[typ]
    self.assertEqual (total, self.amount)

    self.testReorg ()

  def testReorg (self):
    self.mainLogger.info ("Testing a reorg...")
    oldState = self.getGameState ()

    self.rpc.xaya.invalidateblock (self.reorgBlock)
    self.generate (20)

    self.assertEqual (self.isMining ("domob"), False)
    self.assertEqual (self.isMining ("domob 2"), False)
    for c in self.getCharacters ().values ():
      self.assertEqual (c.getFungibleInventory (), {})

    _, remaining = self.getRegionAt (self.pos[0]).getResource ()
    self.assertEqual (remaining, self.amount)

    self.rpc.xaya.reconsiderblock (self.reorgBlock)
    self.expectGameState (oldState)


if __name__ == "__main__":
  MiningTest ().main ()
