import os
import re

UNIT_DIR = os.path.abspath(os.path.dirname(os.path.abspath(__file__)) + '/../src/unit')
TEST_FILE = UNIT_DIR + '/test_files.h'
TEST_PROTOTYPE = '(int (test_[a-zA-Z0-9_]*)\(.*\)).*{'

if __name__ == '__main__':
    with open(TEST_FILE, 'w') as output:
        # Find each test file and collect the test names.
        test_suites = {}
        for root, dirs, files in os.walk(UNIT_DIR):
            for file in files:
                file_path = UNIT_DIR + '/' + file
                if not file.endswith('.c') or file == 'test_main.c':
                    continue
                test_suites[file] = []
                with open(file_path, 'r') as f:
                    for line in f:
                        match = re.match(TEST_PROTOTYPE, line)
                        if match:
                            function = match.group(1)
                            test_name = match.group(2)
                            test_suites[file].append((test_name, function))
        output.write("""typedef int unitTestProc(int argc, char **argv, int flags);

typedef struct unitTest {
    char *name;
    unitTestProc *proc;
} unitTest;

""")

        # Write the headers for the functions
        for file, test_suit in test_suites.items():
            for test in test_suit:
                output.write('{};\n'.format(test[1]))
        output.write("\n")
        
        # Create test suite lists
        for file, test_suit in test_suites.items():
            output.write('unitTest __{}[] = {{'.format(file.replace('.c', '_c')))
            for test in test_suit:
                output.write('{{"{}", {}}}, '.format(test[0], test[0]))
            output.write('{NULL, NULL}};\n')

        output.write("""
struct unitTestSuite {
    char *filename;
    unitTest *tests;
} unitTestSuite[] = {
""")
        for file, test_suit in test_suites.items():
            output.write('    {{"{0}", __{1}}},\n'.format(file, file.replace('.c', '_c')))
        output.write('};\n')