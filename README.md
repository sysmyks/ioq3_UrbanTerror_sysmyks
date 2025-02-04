
# ioq3-update: Custom Urban Terror ioq3 ModJump

## Description

**ioq3-update** is a custom version of the **ioquake3** engine for the **Urban Terror** game, incorporating modifications from **TitanMod**. This project builds upon the latest ioq3 version for Urban Terror 4, adding custom client and server commands to enhance gameplay and server management.

### Sources:
- [ioq3 for Urban Terror 4](https://github.com/FrozenSand/ioq3-for-UrbanTerror-4)
- [UrbanTerror434_mod by BFoxyfox](https://github.com/BFoxyfox/UrbanTerror434_mod)

I started from the latest version and added several **TitanMod** mods to enhance the game experience.

## Custom Client Commands

These custom commands are available to clients in the game:

- **/noclip**: Toggle no-clip mode to fly through walls.
- **/save**: Save your current position or state.
- **/load**: Load the saved position or state.
- **/cg_ghost**: Enable ghost and fix.
- **/help**: Display help for available commands.
- **/svModInfo**: Display information about the current mods on the server.

## Custom Server Commands

These custom commands are designed for server admins and can be executed through RCON (Remote Console):

- **/rcon spoof [player_name] [command]**: Spoof commands to send to clients (useful for administrative tasks).
- **/rcon saveplayerpos [player_num]**: Save the position of a player.
- **/rcon loadplayerpos [player_num] [x] [y] [z] [pitch] [yaw]**: Teleport a player to the specified position (x, y, z coordinates, pitch, and yaw).
- **/rcon serv_infinitestamina [player_name]**: Enable infinite stamina for the specified player.
- **/rcon serv_infinitewalljumps [player_name]**: Enable infinite wall jumps for the specified player.

