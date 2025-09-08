#!/bin/bash

STYLE="$1"

# Set formatting style
if [ "$STYLE" == "kandr" ]; then
    STYLE_OPTION='{BasedOnStyle: LLVM, BreakBeforeBraces: Attach, IndentWidth: 4, ColumnLimit: 160}'
    STYLE="kandr"
else
    STYLE_OPTION='{BasedOnStyle: LLVM, BreakBeforeBraces: Allman, IndentWidth: 4, ColumnLimit: 160, AllowShortIfStatementsOnASingleLine: true}'
    STYLE="allman"
fi

# Find all .cc and .hh files
FILES=$(find . -type f \( -name "*.cc" -o -name "*.hh" \))

if [ -z "$FILES" ]; then
    echo "   No .cc or .hh files found."
    exit 0
fi

# Apply clang-format
echo "💡 Formatting style: $STYLE"
echo "📂 Formatting files..."

for f in $FILES; do
    clang-format -i -style="$STYLE_OPTION" "$f"
    echo "  ✔ Formatted: $f"
done

echo "✅ All files formatted successfully!"
