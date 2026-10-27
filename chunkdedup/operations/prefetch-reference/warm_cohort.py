"""Bounded persistent lookahead assembly; no entry point or production admission.

The caller owns generation/phase guards and persistent catalog readers. Building
does not mutate its Continuation. Commit next_state only after retaining the
cohort receipt; interrupted warming may be retried or explicitly abandoned by
the caller. No worker descriptors, signals, CANCEL or module commands are used.
"""
from __future__ import annotations
from copy import copy
import importlib.util
from pathlib import Path
import sys
import time

_spec = importlib.util.spec_from_file_location(
    'cdsp_cohort_native_window', Path(__file__).with_name('native-window.py'))
nw = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = nw
_spec.loader.exec_module(nw)
p = nw.planner


def build_cohort(planner, state, query, max_records=4096,
                 max_bytes=512 << 20, max_catalog_bytes=8 << 20,
                 preview_check=None):
    """Preview bounded windows, QUERY all pair roles, filter, then physical-sort.

    query(inventory) must return record_key -> verified QUERY response, as
    native_window.Device.query does. Keep this planner and its readers alive
    between calls. The returned next_state retains any giant-group canonical.
    Optional preview_check may refuse before each at-most-8192-row preview.
    """
    if (type(max_records) is not int or not 2 <= max_records <= 819200
            or type(max_bytes) is not int or not 2 <= max_bytes <= 100 << 30
            or type(max_catalog_bytes) is not int
            or not p.ROW_BYTES <= max_catalog_bytes <= 640 << 20):
        raise ValueError('invalid bounded cohort limits')
    started = time.monotonic()
    committed = copy(state)
    start = committed.next_cursor
    plans, inventory, budgets = [], {}, {}
    total_budget = 0
    stop_reason = 'eof'
    while committed.next_cursor < planner.native.total_bytes:
        rows_left = (max_catalog_bytes - (committed.next_cursor - start)) // p.ROW_BYTES
        if rows_left == 0:
            stop_reason = 'catalog_limit'
            break
        if preview_check is not None:
            preview_check()
        preview = copy(committed)
        plan = planner.window(preview, max_rows=min(8192, rows_left), include_pairs=True)
        if preview.next_cursor <= committed.next_cursor:
            raise ValueError('planner made no bounded progress')
        # Preview only this bounded planner window. Copying/summing the accumulated
        # inventory on every preview becomes quadratic at large role counts.
        pending, pending_budgets, new_keys, delta = {}, {}, set(), 0
        for pair in plan['pairs']:
            for role in ('source', 'destination'):
                record, charge = pair[role], pair[role + '_budget_bytes']
                key = p.record_key(record)
                previous = pending_budgets.get(key, budgets.get(key, 0))
                updated = max(charge, previous)
                delta += updated - previous
                pending[key], pending_budgets[key] = record, updated
                if key not in inventory: new_keys.add(key)
        if len(inventory) + len(new_keys) > max_records or total_budget + delta > max_bytes:
            if not plans:
                raise ValueError('first planner window exceeds cohort budget')
            stop_reason = 'inventory_limit'
            break  # Discard preview; retry its rows with committed next_state.
        plans.append(plan)
        inventory.update(pending); budgets.update(pending_budgets)
        total_budget += delta
        committed = preview
    planned = time.monotonic()
    queried = query(list(inventory.values())) if inventory else {}
    after_query = time.monotonic()
    filters, needed, coverage = [], {}, []
    for index, plan in enumerate(plans):
        filtered = p.filter_shared_pairs(plan, queried)
        filters.append(filtered)
        for error in filtered['coverage_errors']:
            coverage.append({'window': index, **error})
        for record in filtered['records']:
            needed[p.record_key(record)] = record
    needed_until = {}
    for plan in plans:
        for pair in plan['pairs']:
            # Reuse the admitted pair decision exactly. An already-shared or
            # uncovered later destination must not extend the lifetime of a
            # source retained for an earlier, still-unshared destination.
            one_pair = p.filter_shared_pairs({**plan, 'pairs': [pair]}, queried)
            if one_pair['coverage_errors'] or not one_pair['needs_verification_pairs']:
                continue
            destination_cursor = pair['destination_catalog_cursor']
            for role in ('source', 'destination'):
                key = p.record_key(pair[role])
                needed_until[key] = max(destination_cursor, needed_until.get(key, 0))
    records = list(needed.values())
    batches = nw.physical_batches(records, queried)
    ended = time.monotonic()
    return {'format': 'CDSP_WARM_COHORT_V1', 'next_state': committed,
            'cursor_start': start, 'cursor_end': committed.next_cursor,
            'catalog_bytes_walked': committed.next_cursor - start,
            'eof': committed.next_cursor == planner.native.total_bytes,
            'stop_reason': stop_reason, 'plans': plans, 'filters': filters,
            'inventory': list(inventory.values()), 'queried': queried,
            'predicted_inventory_bytes': total_budget,
            'records': records, 'batches': batches, 'coverage_errors': coverage,
            'needed_until': needed_until,
            'queried_warm_bytes': sum(queried[p.record_key(r)]['block_size'] for r in records),
            # Geometry may change after QUERY: each actual kernel batch has its
            # independent 256 MiB copied-BP cap, not this predicted byte total.
            'worst_case_issued_logical_bytes': len(batches) * (256 << 20),
            'timing': {'plan': planned-started, 'query': after_query-planned,
                       'filter_sort': ended-after_query, 'total': ended-started},
            'production_admission': False}


def execute_cohort(cohort, device, guard, deadline, now=time.monotonic):
    """Issue finite batches and drain each; stop before the next call on refusal.

    guard() must return True, or {'ok': True, ...}. An optional integer cursor
    omits hints whose last destination is already consumed. A canonical source
    remains eligible until its latest destination; catch-up is cohort END.
    False, exceptions, deadline
    or an invalid warm response prevents all further calls. A deadline is soft:
    an already issued kernel request must drain. It never kills IO or workers.
    """
    started = now()
    results, skipped_records, skipped_batches = [], 0, 0
    status, error = ('PARTIAL_QUERY_COVERAGE', cohort['coverage_errors']) if cohort['coverage_errors'] else ('DRAINED', None)
    # Partial identity/BP coverage is a hold for this cohort, never a successful
    # drainage claim or permission to issue its remaining hints.
    for index, batch in enumerate(cohort['batches'] if not cohort['coverage_errors'] else []):
        if now() >= deadline:
            status = 'DEADLINE'
            break
        try:
            observed = guard()
        except Exception as exc:
            status, error = 'GUARD_ERROR', repr(exc)
            break
        allowed = observed is True or (isinstance(observed, dict) and observed.get('ok') is True)
        if not allowed:
            status, error = 'GUARD_REFUSED', observed
            break
        if isinstance(observed, dict) and 'cursor' in observed:
            cursor = observed['cursor']
            if type(cursor) is not int or cursor < 0:
                status, error = 'GUARD_ERROR', 'invalid observed catalog cursor'
                break
            if cursor >= cohort['cursor_end']:
                status = 'CAUGHT_UP'
                break
            still_needed = [record for record in batch
                            if cohort['needed_until'][p.record_key(record)] > cursor]
            skipped_records += len(batch) - len(still_needed)
            batch = still_needed
            if not batch:
                skipped_batches += 1
                continue
        if now() >= deadline:
            status = 'DEADLINE'
            break
        at = now()
        try:
            response = device.command('warm', records=batch)
            errors = nw.warm_response_errors(response, batch)
        except Exception as exc:
            status, error = 'WARM_ERROR', repr(exc)
            break
        results.append({'batch': index, 'submitted_count': len(batch),
                        'seconds': now()-at, 'guard': observed,
                        'response': response, 'validation_errors': errors})
        if errors:
            status, error = 'WARM_RESPONSE_ERROR', errors
            break
    return {'format': 'CDSP_WARM_EXECUTION_V1', 'status': status, 'error': error,
            'batches_attempted': len(results) + int(status == 'WARM_ERROR'),
            'batches_drained_successfully': sum(not r['validation_errors'] for r in results),
            'batches_total': len(cohort['batches']), 'results': results,
            'obsolete_records_omitted': skipped_records,
            'empty_obsolete_batches_skipped': skipped_batches,
            'seconds': now()-started, 'soft_deadline': deadline,
            'worst_case_issued_logical_bytes':
                (len(results) + int(status == 'WARM_ERROR')) * (256 << 20),
            'coverage_errors': cohort['coverage_errors'],
            'signals_or_cancel_used': False, 'production_admission': False}
