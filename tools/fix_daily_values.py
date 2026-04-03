#!/usr/bin/env python3
"""
Script to move daily portfolio values from April 2 to April 1
to match the deposit date changes.
"""

import struct
import os
import glob
from datetime import datetime
from pathlib import Path

def find_portfolio_files(data_dir):
    """Find all portfolio.dat files in the data directory."""
    pattern = os.path.join(data_dir, '*/portfolio.dat')
    return sorted(glob.glob(pattern))

def read_portfolio(filepath):
    """Read a portfolio file and return version, type, capital, daily values, and transactions."""
    with open(filepath, 'rb') as f:
        # Read header (12 bytes)
        version_bytes = f.read(4)
        type_byte = f.read(1)
        reserved = f.read(3)
        
        version = struct.unpack('<I', version_bytes)[0]
        ptype = struct.unpack('B', type_byte)[0]
        
        # Read capital
        capital_bytes = f.read(8)
        capital = struct.unpack('<d', capital_bytes)[0]
        
        # Read daily values count
        daily_count_bytes = f.read(4)
        daily_count = struct.unpack('<I', daily_count_bytes)[0]
        
        # Read daily values
        daily_values = []
        for _ in range(daily_count):
            date_bytes = f.read(8)
            value_bytes = f.read(8)
            date = struct.unpack('<q', date_bytes)[0]
            value = struct.unpack('<d', value_bytes)[0]
            
            last_updated = None
            if version >= 2:
                updated_bytes = f.read(8)
                last_updated = struct.unpack('<q', updated_bytes)[0]
            
            daily_values.append((date, value, last_updated))
        
        # Read transactions count
        tx_count_bytes = f.read(4)
        tx_count = struct.unpack('<I', tx_count_bytes)[0]
        
        # Read transactions
        transactions = []
        for _ in range(tx_count):
            date_bytes = f.read(8)
            amount_bytes = f.read(8)
            type_bytes = f.read(1)
            symbol_len_bytes = f.read(2)
            
            date = struct.unpack('<q', date_bytes)[0]
            amount = struct.unpack('<d', amount_bytes)[0]
            tx_type = struct.unpack('B', type_bytes)[0]
            symbol_len = struct.unpack('<H', symbol_len_bytes)[0]
            
            symbol = f.read(symbol_len).decode('utf-8') if symbol_len > 0 else ""
            
            shares_bytes = f.read(8)
            shares = struct.unpack('<d', shares_bytes)[0]
            
            notes_len_bytes = f.read(2)
            notes_len = struct.unpack('<H', notes_len_bytes)[0]
            
            notes = f.read(notes_len).decode('utf-8') if notes_len > 0 else ""
            
            transactions.append({
                'date': date,
                'amount': amount,
                'type': tx_type,
                'symbol': symbol,
                'shares': shares,
                'notes': notes
            })
        
        return {
            'version': version,
            'type': ptype,
            'capital': capital,
            'daily_values': daily_values,
            'transactions': transactions
        }

def write_portfolio(filepath, data):
    """Write a portfolio file with the given data."""
    with open(filepath, 'wb') as f:
        # Write header
        f.write(struct.pack('<I', data['version']))
        f.write(struct.pack('B', data['type']))
        f.write(b'\x00\x00\x00')  # reserved
        
        # Write capital
        f.write(struct.pack('<d', data['capital']))
        
        # Write daily values count
        f.write(struct.pack('<I', len(data['daily_values'])))
        
        # Write daily values
        for date, value, last_updated in data['daily_values']:
            f.write(struct.pack('<q', date))
            f.write(struct.pack('<d', value))
            if data['version'] >= 2:
                f.write(struct.pack('<q', last_updated))
        
        # Write transactions count
        f.write(struct.pack('<I', len(data['transactions'])))
        
        # Write transactions
        for tx in data['transactions']:
            f.write(struct.pack('<q', tx['date']))
            f.write(struct.pack('<d', tx['amount']))
            f.write(struct.pack('B', tx['type']))
            f.write(struct.pack('<H', len(tx['symbol'])))
            f.write(tx['symbol'].encode('utf-8'))
            f.write(struct.pack('<d', tx['shares']))
            f.write(struct.pack('<H', len(tx['notes'])))
            f.write(tx['notes'].encode('utf-8'))

def timestamp_to_date(ts):
    """Convert Unix timestamp to date string."""
    return datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')

def date_to_timestamp(year, month, day):
    """Convert date to Unix timestamp (at midnight UTC)."""
    dt = datetime(year, month, day, 0, 0, 0)
    return int(dt.timestamp())

def main():
    data_dir = 'data'
    
    # April 2 and April 1 day boundaries (midnight UTC)
    april_2_start = date_to_timestamp(2026, 4, 2)
    april_2_end = april_2_start + (24 * 3600)
    
    april_1_start = date_to_timestamp(2026, 4, 1)
    april_1_end = april_1_start + (24 * 3600)
    
    print("Updating daily portfolio values from April 2 to April 1\n")
    
    portfolio_files = find_portfolio_files(data_dir)
    
    if not portfolio_files:
        print("No portfolio files found.")
        return
    
    for filepath in portfolio_files:
        portfolio_name = Path(filepath).parent.name
        print(f"Processing {portfolio_name}...")
        
        data = read_portfolio(filepath)
        daily_values = data['daily_values']
        
        # Find daily values on April 2
        april_2_indices = [
            i for i, (date, _, _) in enumerate(daily_values)
            if april_2_start <= date < april_2_end
        ]
        
        if not april_2_indices:
            print(f"  No daily values found on April 2\n")
            continue
        
        # Check if April 1 already has values
        april_1_indices = [
            i for i, (date, _, _) in enumerate(daily_values)
            if april_1_start <= date < april_1_end
        ]
        
        print(f"  Found {len(april_2_indices)} daily value(s) on April 2")
        
        if april_1_indices:
            print(f"  Found {len(april_1_indices)} daily value(s) on April 1")
            print(f"  Keeping April 1 values, removing April 2 values")
            
            # Remove April 2 entries (keep April 1)
            for i in sorted(april_2_indices, reverse=True):
                date, value, last_updated = daily_values[i]
                print(f"    Removing April 2 value: ${value:.2f} at {timestamp_to_date(date)}")
                del daily_values[i]
        else:
            print(f"  No values on April 1, moving April 2 values to April 1")
            
            # Move all April 2 values to April 1 (shift by 1 day = 86400 seconds)
            shift = april_1_start - april_2_start
            
            for i in april_2_indices:
                date, value, last_updated = daily_values[i]
                new_date = date + shift
                
                print(f"    Moving ${value:.2f} from {timestamp_to_date(date)}")
                print(f"                to {timestamp_to_date(new_date)}")
                
                # Update the date
                if last_updated is not None:
                    daily_values[i] = (new_date, value, last_updated)
                else:
                    daily_values[i] = (new_date, value, None)
        
        # Write back
        write_portfolio(filepath, data)
        print(f"  ✓ Updated {portfolio_name}\n")
    
    print("Done!")

if __name__ == '__main__':
    main()
