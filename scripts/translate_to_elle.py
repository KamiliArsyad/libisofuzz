#!/usr/bin/env python3

import sys
import argparse
from collections import defaultdict


def parse_arguments():
    """
    Parses command-line arguments for the translator.
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
    # --- ADD THIS ARGUMENT ---
    parser.add_argument(
        "--filter-prefix",
        nargs='*',  # Allows for zero or more prefixes
        default=['mysql', 'sys.', 'INFORMATION_SCHEMA.', 'PERFORMANCE_SCHEMA.'],
        help="List of table name prefixes to filter out (e.g., 'mysql.' 'sys.')."
    )
    # The --mutate flag is kept for compatibility
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
    id_map = {}
    transactions = defaultdict(lambda: {
        'ops': [],
        'begin_time': -1,
        'end_time': -1,
        'written_objects': set()  # KEY CHANGE: Tracks objects already written to by this trx
    })
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

                _thread_id, trx_id_str, event_type, table, col, row_str, last_writer_id_str = parts

                try:
                    trx_id, row_id, last_writer_id = map(int, [trx_id_str, row_str if row_str != "N/A" else "-1",
                                                               last_writer_id_str])
                except (ValueError, TypeError):
                    continue
                if row_id == -1: row_id = None

                if event_type == "PROMOTE":
                    old_id, new_id = last_writer_id, trx_id
                    id_map[old_id] = new_id
                    if old_id in transactions:
                        transactions[new_id] = transactions.pop(old_id)
                    continue

                canonical_id = id_map.get(trx_id, trx_id)

                if event_type == "BEGIN":
                    if transactions[canonical_id]['begin_time'] == -1:
                        transactions[canonical_id]['begin_time'] = event_time_counter
                elif event_type == "COMMIT":
                    if canonical_id in transactions:
                        transactions[canonical_id]['end_time'] = event_time_counter
                elif event_type in ("READ", "UPDATE", "DELETE", "INSERT"):
                    if table == "N/A" or row_id is None: continue
                    obj_id = (table, row_id)

                    history_at_op_time = object_versions.get(obj_id, [])
                    if event_type == "INSERT":
                        history_at_op_time = []
                    elif not history_at_op_time and last_writer_id != 0:
                        history_at_op_time = [last_writer_id]

                    op_data = {'type': event_type, 'obj_id': obj_id, 'observed_history': history_at_op_time,
                               'value': canonical_id}

                    # For writes, we only add the operation if we haven't already for this object.
                    if event_type in ("INSERT", "UPDATE", "DELETE"):
                        if obj_id not in transactions[canonical_id]['written_objects']:
                            transactions[canonical_id]['ops'].append(op_data)
                            transactions[canonical_id]['written_objects'].add(obj_id)
                        # Always update the global version history, even if we don't log the op
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
    Formats processed data into Elle-compatible EDN strings.
    This version models all writes as a single logical append per row/transaction.
    """
    edn_output = []
    invoke_index = 0
    sorted_trx_ids = sorted(transactions.keys(), key=lambda tid: transactions[tid]['begin_time'])

    for trx_id in sorted_trx_ids:
        data = transactions[trx_id]
        if not data['ops'] or data['end_time'] == -1: continue

        # Check if any operation in this transaction involves a filtered table prefix.
        if any(op['obj_id'][0].startswith(prefix) for op in data['ops'] for prefix in prefixes_to_filter):
            continue  # Skip this entire transaction if it's internal.

        value_ops = []
        for op in data['ops']:
            table_name, row_identifier = op['obj_id']
            elle_key = f"{table_name}-{row_identifier}"

            if op['type'] == 'READ':
                history_str = ' '.join(map(str, op['observed_history']))
                elle_op = f"[:r {elle_key} [{history_str}]]"
            else:  # INSERT, UPDATE, DELETE are coalesced into one logical append
                elle_op = f"[:append {elle_key} {op['value']}]"

            value_ops.append(elle_op)

        value_str = ' '.join(value_ops)
        if not value_str: continue

        begin_time = data['begin_time']
        end_time = data['end_time'] if data['end_time'] != -1 else total_event_count

        invoke = (
            f"{{:type :invoke, :process {trx_id}, :time {begin_time}, :index {invoke_index}, :value [{value_str}]}}")
        ok = (f"{{:type :ok, :process {trx_id}, :time {end_time}, :index {invoke_index + 1}, :value [{value_str}]}}")

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
