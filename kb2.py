
import json
import os
import shutil
from datetime import datetime
from typing import Dict, List, Any, Optional
from dataclasses import dataclass
from pathlib import Path


@dataclass
class KeyProfile:
    id: int
    name: str
    vendor_product_id: int
    layers: List[List[str]]
    rapids: List[int]
    dkss: List[int]
    macros: List[str]
    offsets_v2: List[Dict]
    macronames: List[str]
    checksum: int
    layers_checksum: int
    rapids_checksum: int
    dkss_checksum: int
    macros_checksum: int
    
    @classmethod
    def from_dict(cls, data: Dict) -> 'KeyProfile':
        return cls(
            id=data.get('id', 0),
            name=data.get('name', 'Unknown'),
            vendor_product_id=data.get('vendorProductId', 0),
            layers=data.get('layers', []),
            rapids=data.get('rapids', []),
            dkss=data.get('dkss', []),
            macros=data.get('macros', []),
            offsets_v2=data.get('offsetsV2', []),
            macronames=data.get('macronames', []),
            checksum=data.get('checksum', 0),
            layers_checksum=data.get('layersChecksum', 0),
            rapids_checksum=data.get('rapidsChecksum', 0),
            dkss_checksum=data.get('dkssChecksum', 0),
            macros_checksum=data.get('macrosChecksum', 0)
        )
    
    def to_dict(self) -> Dict:
        return {
            'id': self.id,
            'name': self.name,
            'vendorProductId': self.vendor_product_id,
            'layers': self.layers,
            'rapids': self.rapids,
            'dkss': self.dkss,
            'macros': self.macros,
            'offsetsV2': self.offsets_v2,
            'macronames': self.macronames,
            'checksum': self.checksum,
            'layersChecksum': self.layers_checksum,
            'rapidsChecksum': self.rapids_checksum,
            'dkssChecksum': self.dkss_checksum,
            'macrosChecksum': self.macros_checksum
        }


class ProfileManager:
    
    def __init__(self, file_path: str):
        self.file_path = Path(file_path)
        self.profiles: List[KeyProfile] = []
        self.raw_data: Dict = {}
        self.load()
    
    def load(self) -> bool:
        try:
            if not self.file_path.exists():
                print(f"File not found: {self.file_path}")
                return False
            
            with open(self.file_path, 'r', encoding='utf-8') as f:
                self.raw_data = json.load(f)
            
            self.profiles = []
            for profile_data in self.raw_data.get('profiles', []):
                self.profiles.append(KeyProfile.from_dict(profile_data))
            
            return True
        except Exception as e:
            print(f"Load failed: {e}")
            return False
    
    def save(self, output_path: Optional[Path] = None) -> bool:
        try:
            if output_path is None:
                output_path = self.file_path
            
            self.raw_data['profiles'] = [p.to_dict() for p in self.profiles]
            
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(self.raw_data, f, indent=2, ensure_ascii=False)
            
            return True
        except Exception as e:
            print(f"Save failed: {e}")
            return False
    
    def get_profile(self, profile_id: int) -> Optional[KeyProfile]:
        for p in self.profiles:
            if p.id == profile_id:
                return p
        return None
    
    def get_profile_by_name(self, name: str) -> Optional[KeyProfile]:
        for p in self.profiles:
            if p.name == name:
                return p
        return None
    
    def migrate_profile(self, source_id: int, target_ids: List[int]) -> bool:
        source = self.get_profile(source_id)
        if not source:
            return False
        
        for target_id in target_ids:
            target = self.get_profile(target_id)
            if target and target.id != source_id:
                target.layers = [layer.copy() for layer in source.layers]
                target.rapids = source.rapids.copy()
                target.dkss = source.dkss.copy()
                target.macros = source.macros.copy()
                target.offsets_v2 = [offset.copy() for offset in source.offsets_v2]
                target.macronames = source.macronames.copy()
                target.checksum = source.checksum
                target.layers_checksum = source.layers_checksum
                target.rapids_checksum = source.rapids_checksum
                target.dkss_checksum = source.dkss_checksum
                target.macros_checksum = source.macros_checksum
        
        return True
    
    def export_profile(self, profile_id: int, output_path: Path) -> bool:
        profile = self.get_profile(profile_id)
        if not profile:
            return False
        
        try:
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(profile.to_dict(), f, indent=2, ensure_ascii=False)
            return True
        except Exception as e:
            print(f"Export failed: {e}")
            return False
    
    def export_all(self, output_path: Path) -> bool:
        try:
            if output_path.suffix != '.apjt':
                output_path = output_path.with_suffix('.apjt')
            
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(self.raw_data, f, indent=2, ensure_ascii=False)
            return True
        except Exception as e:
            print(f"Export failed: {e}")
            return False
    
    def import_profile(self, file_path: Path) -> bool:
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            if 'profiles' in data:
                for p_data in data['profiles']:
                    existing = self.get_profile(p_data.get('id', 0))
                    if existing:
                        idx = self.profiles.index(existing)
                        self.profiles[idx] = KeyProfile.from_dict(p_data)
                    else:
                        self.profiles.append(KeyProfile.from_dict(p_data))
            else:
                profile = KeyProfile.from_dict(data)
                existing = self.get_profile(profile.id)
                if existing:
                    idx = self.profiles.index(existing)
                    self.profiles[idx] = profile
                else:
                    self.profiles.append(profile)
            
            return True
        except Exception as e:
            print(f"Import failed: {e}")
            return False


class CLI:
    
    def __init__(self, manager: ProfileManager):
        self.manager = manager
    
    def run(self):
        while True:
            self.print_menu()
            choice = input("Select operation: ").strip()
            
            if choice == "0":
                print("Goodbye!")
                break
            elif choice == "1":
                self.view_profile_detail()
            elif choice == "2":
                self.migrate_profiles()
            elif choice == "3":
                self.export_single_profile()
            elif choice == "4":
                self.export_all_profiles()
            elif choice == "5":
                self.import_profiles()
            elif choice == "6":
                self.rename_profile()
            else:
                print("Invalid choice")
    
    def print_menu(self):
        self.show_profile_list()
        
        print("\nOptions:")
        print("  1. View profile details")
        print("  2. Migrate profile (overwrite)")
        print("  3. Export single profile (JSON)")
        print("  4. Export complete profile (.apjt)")
        print("  5. Import profile")
        print("  6. Rename profile")
        print("  0. Exit")
        print("-"*60)
    
    def show_profile_list(self):
        print("\nProfiles:")
        print("-"*60)
        print(f"{'ID':<6} {'Name':<20} {'Status':<15} {'Layers Chk':<12} {'Rapids Chk'}")
        print("-"*60)
        
        for p in self.manager.profiles:
            has_rapids = any(v != 4294967295 for v in p.rapids[:10])
            has_dkss = any(v != 4294967295 for v in p.dkss[:10])
            
            status = "Complete" if has_rapids else "Template"
            if has_dkss:
                status += " + DKS"
            
            print(f"{p.id:<6} {p.name[:20]:<20} {status:<15} {p.layers_checksum:<12} {p.rapids_checksum}")
    
    def view_profile_detail(self):
        profile = self.select_profile()
        if not profile:
            return
        
        print("\n" + "="*60)
        print(f"Profile Details: {profile.name} (ID: {profile.id})")
        print("="*60)
        
        print(f"ID: {profile.id}")
        print(f"Name: {profile.name}")
        print(f"Vendor Product ID: {profile.vendor_product_id}")
        print(f"Layers count: {len(profile.layers)}")
        print(f"Rapids length: {len(profile.rapids)}")
        print(f"DKS length: {len(profile.dkss)}")
        print(f"Macros count: {len(profile.macros)}")
        print(f"Layers Checksum: {profile.layers_checksum}")
        print(f"Rapids Checksum: {profile.rapids_checksum}")
        print(f"DKS Checksum: {profile.dkss_checksum}")
        print(f"Macros Checksum: {profile.macros_checksum}")
        
        print("\nLayer information:")
        for i, layer in enumerate(profile.layers):
            has_content = any(k != 'KC_TRNS' for k in layer[:20])
            status = "Configured" if has_content else "Passthrough"
            print(f"  Layer {i}: {len(layer)} keys - {status}")
        
        print("\nRapids (first 20):")
        print(f"  {profile.rapids[:20]}")
        
        has_macros = any(m for m in profile.macros)
        print(f"\nMacros: {'Defined' if has_macros else 'Not defined'}")
        
        input("\nPress Enter to continue...")
    
    def select_profile(self, prompt: str = "Select profile") -> Optional[KeyProfile]:
        profiles = self.manager.profiles
        if not profiles:
            print("No profiles available")
            return None
        
        print(f"\n{prompt}:")
        for i, p in enumerate(profiles, 1):
            print(f"  {i}. {p.name} (ID: {p.id})")
        
        try:
            choice = int(input("Enter number: ").strip())
            if 1 <= choice <= len(profiles):
                return profiles[choice - 1]
        except ValueError:
            pass
        
        print("Invalid selection")
        return None
    
    def select_profiles_multi(self, prompt: str = "Select multiple profiles") -> List[KeyProfile]:
        profiles = self.manager.profiles
        if not profiles:
            print("No profiles available")
            return []
        
        print(f"\n{prompt}:")
        for i, p in enumerate(profiles, 1):
            print(f"  {i}. {p.name} (ID: {p.id})")
        
        print("Enter numbers separated by commas (e.g., 1,3,4)")
        choice_str = input("Enter numbers: ").strip()
        
        selected = []
        try:
            for part in choice_str.split(','):
                idx = int(part.strip()) - 1
                if 0 <= idx < len(profiles):
                    selected.append(profiles[idx])
        except ValueError:
            print("Invalid input")
            return []
        
        return selected
    
    def migrate_profiles(self):
        print("\n" + "="*60)
        print("Migrate Profile")
        print("="*60)
        
        source = self.select_profile("Select source profile (will be copied)")
        if not source:
            return
        
        print(f"\nSource: {source.name} (ID: {source.id})")
        print("\nSelect target profiles (will be overwritten):")
        for p in self.manager.profiles:
            if p.id != source.id:
                status = "Has data" if any(v != 4294967295 for v in p.rapids[:10]) else "Empty"
                print(f"  {p.id}. {p.name} - {status}")
        
        choice_str = input("Enter target IDs separated by commas: ").strip()
        target_ids = []
        try:
            for part in choice_str.split(','):
                target_ids.append(int(part.strip()))
        except ValueError:
            print("Invalid input")
            return
        
        print(f"\nWARNING: Will overwrite the following profiles with '{source.name}':")
        for tid in target_ids:
            p = self.manager.get_profile(tid)
            if p:
                print(f"  - {p.name} (ID: {p.id})")
        
        confirm = input("Confirm migration? (y/n): ").strip().lower()
        if confirm == 'y':
            if self.manager.migrate_profile(source.id, target_ids):
                self.manager.save()
                print("Migration successful")
            else:
                print("Migration failed")
        
        input("Press Enter to continue...")
    
    def export_single_profile(self):
        print("\n" + "="*60)
        print("Export Single Profile")
        print("="*60)
        
        profile = self.select_profile("Select profile to export")
        if not profile:
            return
        
        default_name = f"{profile.name}_{profile.id}_{datetime.now().strftime('%Y%m%d')}.json"
        output_path = input(f"Output file path [{default_name}]: ").strip()
        if not output_path:
            output_path = default_name
        
        if self.manager.export_profile(profile.id, Path(output_path)):
            print(f"Profile exported to: {output_path}")
        else:
            print("Export failed")
        
        input("Press Enter to continue...")
    
    def export_all_profiles(self):
        print("\n" + "="*60)
        print("Export Complete Profile")
        print("="*60)
        
        default_name = f"archon_backup_{datetime.now().strftime('%Y%m%d_%H%M%S')}.apjt"
        output_path = input(f"Output file path (.apjt) [{default_name}]: ").strip()
        if not output_path:
            output_path = default_name
        
        if not output_path.endswith('.apjt'):
            output_path += '.apjt'
        
        if self.manager.export_all(Path(output_path)):
            print(f"Complete profile exported to: {output_path}")
        else:
            print("Export failed")
        
        input("Press Enter to continue...")
    
    def import_profiles(self):
        print("\n" + "="*60)
        print("Import Profile")
        print("="*60)
        
        import_path = input("Import file path (.json or .apjt): ").strip()
        if not Path(import_path).exists():
            print("File not found")
            input("Press Enter to continue...")
            return
        
        if self.manager.import_profile(Path(import_path)):
            self.manager.save()
            print("Import successful")
        else:
            print("Import failed")
        
        input("Press Enter to continue...")
    
    def rename_profile(self):
        print("\n" + "="*60)
        print("Rename Profile")
        print("="*60)
        
        profile = self.select_profile("Select profile to rename")
        if not profile:
            return
        
        new_name = input(f"New name [{profile.name}]: ").strip()
        if new_name and new_name != profile.name:
            old_name = profile.name
            profile.name = new_name
            self.manager.save()
            print(f"Renamed: {old_name} -> {new_name}")
        else:
            print("Name not changed")
        
        input("Press Enter to continue...")


def main():
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: python profile_manager.py <config_file_path>")
        print("Example: python profile_manager.py AllProfiles.json")
        sys.exit(1)
    
    file_path = sys.argv[1]
    
    manager = ProfileManager(file_path)
    if not manager.load():
        print("Failed to load configuration file")
        sys.exit(1)
    
    cli = CLI(manager)
    cli.run()


if __name__ == "__main__":
    main()