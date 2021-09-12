#!/bin/bash
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
clang-format -i -style=file "$REPO"/Source/*.cpp "$REPO"/Source/*.h
