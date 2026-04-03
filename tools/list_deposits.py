#!/usr/bin/env python3
"""List all deposits in all portfolios with their dates."""

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

def tx_type_name(tx_type):
    """Get transaction type name."""
    types = {0: 'DEPOSIT', 1: 'WITHDRAWAL', 2: 'BUY_STOCK', 3: 'SELL_STOCK', 4: 'DIVIDEND'}
    return types.get(tx_type, f'UNKNOWN({tx_type})')

def timestamp_to_date(ts):
    """Convert Unix timestamp to date string."""
    return datetime.fromtimestamp(ts).strftime('%Y-%m-%d %H:%M:%S')

def main():
    data_dir = 'data'
    
    portfolio_files = find_portfolio_files(data_dir)
    
    if not portfolio_files:
        print("No portfolio files found.")
        return
    
    for filepath in portfolio_files:
        portfolio_name = Path(filepath).parent.name
        print(f"\n{portfolio_name}:")
        print("-" * 60)
        
        data = read_portfolio(filepath)
        transactions = data['transactions']
        
        deposits = [tx for tx in transactions if tx['type'] == 0]
        
        if not deposits:
            print("  No deposits found")
            continue
        
        for tx in deposits:
            date_str = timestamp_to_date(tx['date'])
            print(f"  {date_str:20}  ${tx['amount']:>10.2f}  {tx['notes']}")

if __name__ == '__main__':
    main()
