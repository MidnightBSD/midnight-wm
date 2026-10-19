// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
  const std::string_view command = argv[argc - 1];
  if (command == "read_node_list") std::cout << "Name ID Num hooks\nubt0hci 00000001 2\nubt1hci 00000002 2\n";
  else if (command == "read_connection_list") std::cout << "Remote BD_ADDR Handle Type Mode Role Encrypt Pending Queue State\n01:02:03:04:05:06 42 ACL 0 MAST NONE 0 0 OPEN\n";
  else if (command == "read_neighbor_cache") std::cout << "T BD_ADDR Features\nB 11:22:33:44:55:66 00 00\n";
  else if (command == "inquiry" && argc > 3 && std::string_view(argv[3]) == "ubt0hci") { std::cout << "BD_ADDR: aa:bb:cc:dd:ee:ff\n"; return 0; }
  else if (command == "inquiry") { std::cerr << "Permission denied\n"; return 1; }
  else return 2;
}
