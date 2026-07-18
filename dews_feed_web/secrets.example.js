// Copy to secrets.js and fill in. secrets.js is untracked and is deployed
// to the Pi alongside index.html by deploy_dashboard.ps1.
window.SECRETS={
  owmKey:"YOUR_OPENWEATHERMAP_KEY",
  fredKey:"YOUR_FRED_KEY",
  gmapsKey:"YOUR_GOOGLE_MAPS_KEY",
  todewToken:"YOUR_TODEW_SYNC_TOKEN",       // = ToDew Worker HEALTH_TOKEN
  whoopRelay:"YOUR_APPS_SCRIPT_RELAY_URL",  // WHOOP relay, or "" to hide page
  avKey:"YOUR_ALPHA_VANTAGE_KEY",
};
