#!/usr/bin/env python3

import sys
import argparse
from collections import defaultdict


def parse_arguments():
    """
    Parses command-line arguments for the translator, including filters.
    """
    parser = argparse.ArgumentParser(
        description="Translate an IsoFuzz trace log into an Elle-compatible EDN format."
    )
    parser.add_argument(
        "trace_file",
        type=str,
        help="Path to the input IsoFuzz trace file."
    )
    parser.add_argument(
        "output_file",
        type=str,
        help="Path where the output EDN file will be written."
    )
    parser.add_argument(
        "--filter-prefix",
        nargs='*',  # Allows for zero or more prefixes
        default=['mysql', 'sys.', 'INFORMATION_SCHEMA.', 'PERFORMANCE_SCHEMA.'],
        help="List of table name prefixes to filter out (e.g., 'mysql.' 'sys.')."
    )
    parser.add_argument(
        "--mutate",
        nargs='?',
        const=1,
        type=int,
        help="Enable mutation (currently a placeholder for compatibility)."
    )
    return parser.parse_args()


def process_log_file(trace_file_path):
    """
    Reads the raw log file and processes it in a single pass.
    This version correctly handles PROMOTE events and coalesces multiple physical
    writes to a single row within a transaction into a single logical write event.
    """
    # Maps a temporary transaction ID to its final, permanent ID. e.g., {382: 444486}
    id_map = {}

    # The primary data structure. Maps a canonical transaction ID to its data.
    transactions = defaultdict(lambda: {
        'ops': [],
        'begin_time': -1,
        'end_time': -1,
        'written_objects': set()  # Tracks (table, row) tuples already written by this trx.
    })

    # Globally tracks the version history for each row-level object.
    # Structure: { (table_name, row_id): [ver1_trx_id, ver2_trx_id, ...] }
    object_versions = defaultdict(list)
    event_time_counter = 0

    try:
        with open(trace_file_path, 'r') as f:
            for line in f:
                event_time_counter += 1
                line = line.strip()
                if not line: continue
                parts = line.strip().split('\t')
                if len(parts) != 7: continue

                _thread_id, trx_id_str, event_type, table, _col, row_str, last_writer_id_str = parts

                try:
                    trx_id = int(trx_id_str)
                    row_id = int(row_str) if row_str != "N/A" else None
                    last_writer_id = int(last_writer_id_str)
                except (ValueError, TypeError):
                    continue

                # Handle PROMOTE as a special directive to update our ID map
                if event_type == "PROMOTE":
                    old_id, new_id = last_writer_id, trx_id
                    id_map[old_id] = new_id
                    if old_id in transactions:
                        transactions[new_id] = transactions.pop(old_id)
                    continue

                # Resolve the canonical transaction ID for the current event
                canonical_id = id_map.get(trx_id, trx_id)

                if event_type == "BEGIN":
                    if transactions[canonical_id]['begin_time'] == -1:
                        transactions[canonical_id]['begin_time'] = event_time_counter
                elif event_type == "COMMIT":
                    if canonical_id in transactions:
                        transactions[canonical_id]['end_time'] = event_time_counter
                elif event_type in ("READ", "UPDATE", "DELETE", "INSERT"):
                    if table == "N/A" or row_id is None: continue

                    # The object of atomicity is the row.
                    obj_id = (table, row_id)

                    # Get the correct prefix of the version history for this operation.
                    full_history = object_versions.get(obj_id, [])
                    observed_history = []
                    if event_type in ("READ", "UPDATE", "DELETE"):
                        try:
                            # A read observes the history UP TO the version it read.
                            idx = full_history.index(last_writer_id)
                            observed_history = full_history[:idx + 1]
                        except ValueError:
                            # This fixes the empty-read bug. If the version isn't in our
                            # tracked history, it must be the initial version of the object.
                            if last_writer_id != 0:
                                observed_history = [last_writer_id]
                    # For INSERT, observed_history remains an empty list, which is correct.

                    op_data = {
                        'type': event_type,
                        'obj_id': obj_id,
                        'observed_history': observed_history,
                        'value': canonical_id
                    }

                    if event_type in ("INSERT", "UPDATE", "DELETE"):
                        # --- "WRITE-ONCE" COALESCING LOGIC ---
                        # Only append the logical write operation if we haven't already for this object.
                        if obj_id not in transactions[canonical_id]['written_objects']:
                            transactions[canonical_id]['ops'].append(op_data)
                            transactions[canonical_id]['written_objects'].add(obj_id)

                        # Always update the global version history to reflect the physical write.
                        object_versions[obj_id].append(canonical_id)
                    else:  # It's a READ
                        transactions[canonical_id]['ops'].append(op_data)

    except FileNotFoundError:
        print(f"Error: Trace file not found at '{trace_file_path}'", file=sys.stderr)
        return None, 0
    except Exception as e:
        print(f"An unexpected error occurred: {e}", file=sys.stderr)
        return None, 0

    return transactions, event_time_counter


def format_as_edn(transactions, total_event_count, prefixes_to_filter):
    """
    Formats processed data into Elle-compatible EDN strings, correctly
    separating :invoke and :ok values and filtering internal tables.
    """
    edn_output = []
    invoke_index = 0
    sorted_trx_ids = sorted(transactions.keys(), key=lambda tid: transactions[tid]['begin_time'])

    for trx_id in sorted_trx_ids:
        data = transactions[trx_id]
        if not data['ops'] or data['end_time'] == -1: continue

        # Filter out internal transactions that only touch system tables.
        if any(op['obj_id'][0].startswith(tuple(prefixes_to_filter)) for op in data['ops']):
            continue

        # Build two separate lists for invoke and ok operations
        invoke_ops = []
        ok_ops = []

        for op in data['ops']:
            table_name, row_identifier = op['obj_id']
            elle_key = f"{table_name}-{row_identifier}"

            if op['type'] == 'READ':
                # For a read, invoke is the request (:r key nil).
                invoke_ops.append(f"[:r {elle_key} nil]")
                # And ok is the response (:r key [version-list]).
                history_str = ' '.join(map(str, op['observed_history']))
                ok_ops.append(f"[:r {elle_key} [{history_str}]]")
            else:  # INSERT, UPDATE, DELETE are coalesced into one logical append.
                elle_op = f"[:append {elle_key} {op['value']}]"
                # For writes, the operation is the same in both invoke and ok.
                invoke_ops.append(elle_op)
                ok_ops.append(elle_op)

        invoke_value_str = ' '.join(invoke_ops)
        ok_value_str = ' '.join(ok_ops)

        if not invoke_value_str: continue

        begin_time = data['begin_time']
        end_time = data['end_time'] if data['end_time'] != -1 else total_event_count

        invoke = (f"{{:type :invoke, :process {trx_id}, :time {begin_time}, "
                  f":index {invoke_index}, :value [{invoke_value_str}]}}")
        ok = (f"{{:type :ok, :process {trx_id}, :time {end_time}, "
              f":index {invoke_index + 1}, :value [{ok_value_str}]}}")

        edn_output.append(invoke)
        edn_output.append(ok)
        invoke_index += 2

    return edn_output


def main():
    """ Main execution function. """
    args = parse_arguments()

    if args.mutate:
        print("Warning: --mutate flag is recognized but mutation logic is not implemented.", file=sys.stderr)

    processed_transactions, total_events = process_log_file(args.trace_file)
    if processed_transactions is None:
        sys.exit(1)

    edn_lines = format_as_edn(processed_transactions, total_events, args.filter_prefix)

    try:
        with open(args.output_file, 'w') as f:
            for line in edn_lines:
                f.write(line + '\n')
    except IOError as e:
        print(f"Error writing to output file '{args.output_file}': {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
