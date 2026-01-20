# Backup CLI tool

A Linux directory mirroring tool that keeps one or more backup targets in sync
with a source directory. It performs an initial copy and then monitors the
source with inotify to reflect creates, updates, moves, and deletions.

## Features
- Live mirroring from a source directory to one or more targets.
- Tracks directory moves and renames to avoid full re-copy.
- Mirrors regular files and symlinks (rewriting absolute symlinks pointing into
  the source so they stay valid in the backup).
- Restore mode to revert the source back to a selected backup snapshot.
- Multiple concurrent backups from the same source.

## Requirements
- Linux.
- `make` and a C compiler (tested with `cc`).

## Build
Build a project in the project directory with ```make```

The binary is `sop-backup`.

## Usage
Run the program and use the interactive commands:
```
./sop-backup
```

Commands:
- `add <source> <target1> [target2 ...]`  
  Start mirroring `source` into each target directory.
- `end <source> <target1> [target2 ...]`  
  Stop mirroring. The backup directory remains for restore.
- `list`  
  Show active and ended backups.
- `restore <source> <target>`  
  Restore `source` from the backup in `target`.
- `exit`  
  Quit and stop active backups.

Example session:
```
add /home/user/data /home/user/data.backup
list
end /home/user/data /home/user/data.backup
restore /home/user/data /home/user/data.backup
exit
```

## Notes and behavior
- The source must be an existing directory.
- Targets must be empty directories or non-existent paths; the program creates
  them as needed.
- `restore` removes items from the source that are not present in the backup
  and overwrites items that have been modified since the backup was created.

