#!/bin/bash
#/*
# * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
# * contributor license agreements.  See the NOTICE file distributed with
# * this work for additional information regarding copyright ownership.
# * The OpenAirInterface Software Alliance licenses this file to You under
# * the OAI Public License, Version 1.1  (the "License"); you may not use this file
# * except in compliance with the License.
# * You may obtain a copy of the License at
# *
# *      http://www.openairinterface.org/?page_id=698
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# *-------------------------------------------------------------------------------
# * For more information about the OpenAirInterface (OAI) Software Alliance:
# *      contact@openairinterface.org
# */

# ==============================================================================
# SCRIPT CONFIGURATION
# ==============================================================================

# Set the maximum number of files allowed to have formatting violations.
# The script will exit with a non-zero status if this number is exceeded.
MAX_FORMAT_VIOLATIONS=869

# Define the file types to check.
# The script will search for these file extensions recursively from the current directory.
# You can add or remove file extensions as needed.
FILE_EXTENSIONS=("*.cpp" "*.cxx" "*.cc" "*.hpp" "*.h" "*.hxx")

# ==============================================================================
# SCRIPT LOGIC
# ==============================================================================

# Initialize a counter for files with formatting violations.
files_with_violations=0
# A temporary variable to track the exit status of commands.
exit_status=0

echo "Starting clang-format check..."
echo "Maximum allowed violations: $MAX_FORMAT_VIOLATIONS"
echo "----------------------------------------"

# Loop through all specified file types and check them.
for ext in "${FILE_EXTENSIONS[@]}"; do
    # Use 'find' to get a list of all files with the current extension.
    # The 'eval' command is used to correctly handle the globbing of file extensions.
    # List of directories to exclude from the search
    EXCLUDE_DIRS=("cmake_targets/build" "cmake_targets/ran_build" "openair2/E2AP/flexric") # Replace with actual directory names

    # Build the prune expression for excluded directories
    prune_expr=""
    for exclude in "${EXCLUDE_DIRS[@]}"; do
        prune_expr="$prune_expr -path \"./$exclude\" -prune -o"
    done

    # Construct the find command with exclusions
    find_command="find . $prune_expr -type f -name \"$ext\" -print"
    # Execute the find command and read each file path.
    while IFS= read -r file; do
        # Use clang-format to format the file and pipe the output to 'diff'.
        # The 'diff -q' command compares the file with the formatted output.
        # It exits with status 1 if a difference is found (i.e., a violation).
        # We redirect stdout and stderr to /dev/null to keep the output clean.
        clang-format -style=file "$file" | diff -q "$file" - >/dev/null 2>&1
        exit_status=$?

        # Check if 'diff' found a difference.
        if [ $exit_status -eq 1 ]; then
            echo "  [VIOLATION] $file"
            # Increment the violation counter if a difference is found.
            files_with_violations=$((files_with_violations + 1))
        fi

    done < <(eval "$find_command")
done

echo "----------------------------------------"
echo "Check complete."
echo "Total files with formatting violations: $files_with_violations"
echo "Maximum allowed violations: $MAX_FORMAT_VIOLATIONS"

# Check if the number of violations exceeds the allowed maximum.
if [ "$files_with_violations" -gt "$MAX_FORMAT_VIOLATIONS" ]; then
    echo "----------------------------------------"
    echo "FAILURE: The number of formatting violations ($files_with_violations) " \
         "exceeds the maximum allowed ($MAX_FORMAT_VIOLATIONS)."
    exit 1
else
    echo "----------------------------------------"
    echo "SUCCESS: The number of formatting violations satisfy requirement"
    exit 0
fi
