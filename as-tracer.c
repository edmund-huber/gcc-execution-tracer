#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(cond) \
    { \
        if (!(cond)) { \
            printf("ASSERT(%s) failed at %s L%i\n", #cond, __FILE__, __LINE__); \
            exit(1); \
        } \
    }

#define NAME "as-tracer"

int nonce = 0;

int skip_exactly(char *to_skip, char **s) {
    // If *s begins with to_skip, progress *s past to_skip and return 1.
    if (strncmp(*s, to_skip, strlen(to_skip)) == 0) {
        *s += strlen(to_skip);
        return 1;
    }
    // Else return 0.
    return 0;
}

int scan_until(char c, char **s, char *scanned, int scanned_sz) {
    char *original_s = *s;
    // Until we've reached the end of *s,
    int scanned_off = 0;
    scanned[0] = '\0';
    for (; **s != '\0'; *s += 1) {
        // .. if we found c, then success.
        if (**s == c) {
            return 1;
        }
        // Store the characters we're skipping in scanned.
        if (scanned != NULL) {
            if (scanned_off + 1 == scanned_sz) {
                return 0;
            }
            scanned[scanned_off] = **s;
            scanned[scanned_off + 1] = '\0';
            scanned_off++;
        }
    }
    // If we did not find c in *s, then we need to restore *s to where it was
    // at the start.
    *s = original_s;
    return 0;
}

// Reference: https://en.wikibooks.org/wiki/X86_Assembly/Control_Flow .
char *x86_64_branching_inst[] = {
    // Unconditional jump.
    "jmp",
    // Jump based on status flags.
    "je", "jne", "jg", "jge", "ja", "jae", "jl", "jle", "jb", "jbe", "jo",
    "jno", "jz", "jnz", "js", "jns",
    // Conditional jump based on {,e,r}cx registers.
    "jcxz", "jecxz", "jrcxz",
    // Loop instructions.
    "loop", "loope", "loopne", "loopnz", "loopz",
    // Function call and return.
    "call", "ret",
    NULL
};

int main(int argc, char **argv) {
    // Parse the command line.
    char *output_fn = NULL;
    int is_64 = 0;
    int c;
    char *long_option_64 = "64";
    struct option long_options[] = {
        { long_option_64, no_argument, NULL, 0 },
        { 0, 0, 0, 0}
    };
    int long_index;
    while ((c = getopt_long(argc, argv, "o:", long_options, &long_index)) != -1) {
        switch (c) {
        case 0:
            if (long_options[long_index].name == long_option_64) {
                is_64 = 1;
            }
            break;
        case 'o':
            output_fn = optarg;
            break;
        default:
            ASSERT(0);
        }
    }

    // Check that we got the flags we were expecting.
    ASSERT(is_64);
    ASSERT(output_fn != NULL);

    // In addition to flags, we take one more option: the path to the assembler
    // file.
    ASSERT(optind == argc - 1);
    char *input_fn = argv[optind];

    // Set up the temporary file that we will write the instrumented assembly
    // to, (which we'll later use gas to assemble).
    char temp_fn[] = "/tmp/XXXXXX";
    int temp_fd = mkstemp(temp_fn);
    ASSERT(temp_fd != -1);
    FILE *temp_f = fdopen(temp_fd, "w");

    // The following is the state of the parser, which works in a single pass:
    // On the first line, we are expecting a .file directive.
    int first_line = 1;
    int dont_instrument = 0;
    char source_fn[128] = { 0 };
    int previous_line_no;
    char source_buffer[1024 * 1024];

    FILE *in_file = fopen(input_fn, "r");
    char line[128];
    while (fgets(line, sizeof(line), in_file) != NULL) {
        ASSERT(strlen(line) > 0);
        ASSERT((line[strlen(line) - 1] == '\n') || feof(in_file));

        // If the first line is "# as-tracer-ignore" then we won't instrument
        // the file.
        char *s = line;
        dont_instrument = dont_instrument || (
            first_line &&
            skip_exactly("# as-tracer-ignore\n", &s)
        );
        if (dont_instrument) {
            goto done_with_line;
        }

        // Try to parse out a .file directive, they look like this:
        // 	.file	"pretzel.c"
        s = line;
        int found_file_directive =
            skip_exactly("\t.file\t\"", &s) &&
            scan_until('"', &s, source_fn, sizeof(source_fn)) &&
            skip_exactly("\"\n", &s) &&
            *s == '\0';
        if (found_file_directive) {
            printf(NAME ": instrumenting '%s'\n", source_fn);
            previous_line_no = -1;
            source_buffer[0] = '\0';
            goto done_with_line;
        } else {
            // Expecting the .file directive on the first line.
            ASSERT(!first_line);
        }

        // Try to parse out a comment generated by -fverbose-asm, they look
        // like this:
        // # pretzel.c:6:     if (argc != 2) {
        s = line;
        char found_source_fn[128] = { 0 };
        char found_line_no[32] = { 0 };
        char found_line[1024] = { 0 };
        int found_verbose_asm_comment =
            skip_exactly("# ", &s) &&
            scan_until(':', &s, found_source_fn, sizeof(found_source_fn)) &&
            skip_exactly(":", &s) &&
            scan_until(':', &s, found_line_no, sizeof(found_line_no)) &&
            skip_exactly(":", &s) &&
            scan_until('\n', &s, found_line, sizeof(found_line)) &&
            skip_exactly("\n", &s) &&
            *s == '\0';
        if (found_verbose_asm_comment) {
            // If this isn't the same source file called out in the .file
            // directive, then we are extremely confused.
            ASSERT(strcmp(source_fn, found_source_fn) == 0);

            // If this is the same line number as a verbose-asm comment that
            // we've already seen, just keep going.
            int line_no = atoi(found_line_no);
            if (line_no == previous_line_no)
                goto done_with_line;

            // Otherwise, let's add it to source_buffer.
            ASSERT(sizeof(source_buffer) > strlen(source_buffer) + strlen(found_line));
            ASSERT(snprintf(
                source_buffer + strlen(source_buffer), sizeof(source_buffer) - strlen(source_buffer),
                "# %i: %s\n", line_no, found_line
            ) < sizeof(source_buffer) - strlen(source_buffer));

            previous_line_no = line_no;
            goto done_with_line;
        }

        // If we come across a jmp, call, ret, (etc) -- any instruction that
        // causes the instruction pointer to change -- let's insert the "record
        // stub".
        s = line;
        if (skip_exactly("\t", &s)) {
            for (int i = 0; x86_64_branching_inst[i] != NULL; i++) {
                if (skip_exactly(x86_64_branching_inst[i], &s)) {
                    // Print what we'd like to record directly to the assembly
                    // -- for debugging purposes.
                    fprintf(temp_f, "# WANT TO RECORD: %s\n", source_fn);
                    fputs(source_buffer, temp_f);

                    // Copy the record stub in.
                    ASSERT(fputs("# BEGIN RECORD STUB\n", temp_f) > 0);
                    FILE *stub_f = fopen("asm/x86_64_record_stub.s", "r");
                    char stub_line[128];
                    char stub_line_part[128];
                    while (fgets(stub_line, sizeof(stub_line), stub_f) != NULL) {
                        ASSERT(strlen(stub_line) > 0);
                        ASSERT((stub_line[strlen(stub_line) - 1] == '\n') || feof(stub_f));

                        char *stub_s = stub_line;
                        while (scan_until('?', &stub_s, stub_line_part, sizeof(stub_line_part))) {
                            ASSERT(fputs(stub_line_part, temp_f) > 0);
                            ASSERT(skip_exactly("?", &stub_s));
                            ASSERT(scan_until('?', &stub_s, stub_line_part, sizeof(stub_line_part)));
                            if (strcmp(stub_line_part, "NONCE") == 0) {
                                fprintf(temp_f, "%i", nonce);
                            } else if (strcmp(stub_line_part, "TRACE_BLOCK_ID") == 0) {
                                fprintf(temp_f, "$%i", 99);
                            } else {
                                ASSERT(0);
                            }
                            ASSERT(skip_exactly("?", &stub_s));
                        }
                        ASSERT(fputs(stub_s, temp_f) > 0);
                    }
                    nonce++;
                    fclose(stub_f);
                    ASSERT(fputs("# END RECORD STUB\n", temp_f) > 0);

                    previous_line_no = -1;
                    source_buffer[0] = '\0';
                    goto done_with_line;
                }
            }
        }

        // If we come across any use of the r15 register, then the -ffixed-r15
        // flag didn't work, and we can't continue.
        ASSERT(strstr(line, "%r15") == NULL);

    done_with_line:
        first_line = 0;
        fputs(line, temp_f);
    }
    fclose(temp_f);

    // If we run past the end of the assembly source, and we have anything left
    // in 'source_buffer', something has gone really wrong, because any
    // sensible assembly file should end with a 'ret' instruction ..
    ASSERT(strlen(source_buffer) == 0);

    // Use gas to assemble our instrumented assembly, (and clean up after
    // ourselves).
    char command[128] = { 0 };
    snprintf(command, sizeof(command), "as --64 -o %s %s", output_fn, temp_fn);
    int ret = system(command);
    unlink(temp_fn);

    return ret;
}
