import { render } from "preact";
import { App } from "./App";
import { loadAuth } from "./store";
import "./kit/tokens.css";
import "./kit/base.css";
import "./kit/zigbee.css";
import "./bridge.css";

void loadAuth();
render(<App />, document.getElementById("app")!);
