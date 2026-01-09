# An awk script to determine the type of a file.
/^\177ELF\001/      { if (NR == 1) { print "elf32"; exit } }
/^\177ELF\002/      { if (NR == 1) { print "elf64"; exit } }
/^\114\001/         { if (NR == 1) { print "pecoff"; exit } }
/^\144\206/         { if (NR == 1) { print "pecoff"; exit } }
/^\000\000\377\377/ { if (NR == 1) { print "pecoff"; exit } }
/^\001\337/         { if (NR == 1) { print "xcoff32"; exit } }
/^\001\367/         { if (NR == 1) { print "xcoff64"; exit } }
/^\376\355\372\316/ { if (NR == 1) { print "macho"; exit } }
/^\316\372\355\376/ { if (NR == 1) { print "macho"; exit } }
/^\376\355\372\317/ { if (NR == 1) { print "macho"; exit } }
/^\317\372\355\376/ { if (NR == 1) { print "macho"; exit } }
/^\312\376\272\276/ { if (NR == 1) { print "macho"; exit } }
/^\276\272\376\312/ { if (NR == 1) { print "macho"; exit } }
