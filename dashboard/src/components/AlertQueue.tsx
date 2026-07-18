import type { Alert } from "../types";
import AlertCard from "./AlertCard";

interface Props {
  alerts: Alert[];
  onStatusChanged: () => void;
  onSelectInstrument: (instrumentId: string) => void;
  selectedInstrument: string;
}

export default function AlertQueue({ alerts, onStatusChanged, onSelectInstrument, selectedInstrument }: Props) {
  if (alerts.length === 0) {
    return <p className="empty-state">No alerts match the current filters yet.</p>;
  }
  return (
    <div>
      {alerts.map((alert) => (
        <AlertCard
          key={alert.alert_id}
          alert={alert}
          onStatusChanged={onStatusChanged}
          onSelectInstrument={onSelectInstrument}
          selected={alert.instrument_id === selectedInstrument}
        />
      ))}
    </div>
  );
}
