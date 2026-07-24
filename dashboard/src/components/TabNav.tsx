import { TABS, type Tab } from "../tabs";
import "./TabNav.css";

interface Props {
  active: Tab;
  onChange: (tab: Tab) => void;
}

export default function TabNav({ active, onChange }: Props) {
  return (
    <nav className="tab-nav">
      {TABS.map((tab) => (
        <button
          key={tab}
          type="button"
          className={tab === active ? "tab-active" : ""}
          aria-current={tab === active ? "page" : undefined}
          onClick={() => onChange(tab)}
        >
          {tab}
        </button>
      ))}
    </nav>
  );
}
