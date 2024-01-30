import sys
import random

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} [output_dir]")
    exit(0)

filename = "../raw_dataset/all_csv_files.csv"
output_dir = sys.argv[1]

NUM_THREADS=16
INIT_KEYS=10_000_000
PER_THREAD_KEYS=40_000_000
KEY_LENGTH=12

for WORKLOAD in ["D", "E"]:
    current_state="load"
    counter = 0
    current_thread = 0

    current_file = open(f"{output_dir}/Workload{WORKLOAD}/workload_{WORKLOAD}_load", "w")
    worker_files = [open(f"{output_dir}/Workload{WORKLOAD}/workload_{WORKLOAD}_worker_{i}", "w") for i in range(NUM_THREADS)]

    key_buffer = list() # Used for workload D
    inserted_keys = list() # Used for workload E
    def generate_op(workload, current_key):
        global key_buffer
        if workload == "D":
            random_num = random.random()
            if random_num < 0.9:
                if len(key_buffer) == 0:
                    return "r", current_key
                else:
                    random_int = random.randrange(0, len(key_buffer))
                    return "r", key_buffer[random_int]
            else:
                if len(key_buffer) == 10:
                    key_buffer.pop(0)
                key_buffer.append(current_key)
                return "i", current_key
        elif workload == "E":
            random_num = random.random()
            if random_num < 0.9:
                random_int = random.randrange(0, len(inserted_keys))
                return "s", inserted_keys[random_int]
            else:
                inserted_keys.append(current_key)
                return "i", current_key


    with open(filename, "r") as f:
        while True:
            line = f.readline()
            if line == "":
                break
            else:
                splitted = line.split(",")
                key = splitted[1]
                key = key + "0" * (KEY_LENGTH - len(key))

                if current_state == "load":
                    counter += 1
                    if counter == INIT_KEYS + 1:
                        counter = 0
                        current_state = "thread"
                        current_file.close()
                    else:
                        inserted_keys.append(key[1:KEY_LENGTH+1])
                        current_file.write(f"{key[1:KEY_LENGTH+1]}\n")

                elif current_state == "thread":
                    if current_thread == NUM_THREADS:
                        current_thread = 0
                        counter += 1
                        if counter == PER_THREAD_KEYS:
                            break
                    op, query_key = generate_op(WORKLOAD, key)
                    worker_files[current_thread].write(f"{op} {query_key[1:KEY_LENGTH+1]}\n")
                    current_thread += 1

    for i in range(NUM_THREADS):
        worker_files[i].close()