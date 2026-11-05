// Phone side of Vesta: opens the hosted settings page, relays saved settings to
// the watch, and fetches weather for the phone's current location on request.
var CONFIG_URL = 'https://vesta.markhenke.com';

// Open-Meteo weather codes as board text: at most 8 characters, with a closing
// period when a word had to be abbreviated.
var WEATHER_WORDS = {
  0: 'CLEAR', 1: 'CLEAR', 2: 'PT CLDY.', 3: 'OVERCAST',
  45: 'FOG', 48: 'FOG',
  51: 'DRIZZLE', 53: 'DRIZZLE', 55: 'DRIZZLE', 56: 'FRZ DRZ.', 57: 'FRZ DRZ.',
  61: 'LIGHT R.', 63: 'RAIN', 65: 'HEAVY R.', 66: 'FRZ RN.', 67: 'FRZ RN.',
  71: 'LIGHT S.', 73: 'SNOW', 75: 'HEAVY S.', 77: 'SNOW GR.',
  80: 'SHOWERS', 81: 'SHOWERS', 82: 'SHOWERS', 85: 'SNOW SH.', 86: 'SNOW SH.',
  95: 'THUNDER.', 96: 'HAIL', 99: 'HAIL'
};
var WEATHER_FAILED = 'NO DATA';

function sendWeather(text) {
  Pebble.sendAppMessage({ WeatherText: text });
}

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(function(pos) {
    var xhr = new XMLHttpRequest();
    xhr.onload = function() {
      try {
        var code = JSON.parse(this.responseText).current.weather_code;
        sendWeather(WEATHER_WORDS[code] || WEATHER_FAILED);
      } catch (e) {
        sendWeather(WEATHER_FAILED);
      }
    };
    xhr.onerror = function() {
      sendWeather(WEATHER_FAILED);
    };
    xhr.open('GET', 'https://api.open-meteo.com/v1/forecast?latitude=' + pos.coords.latitude +
             '&longitude=' + pos.coords.longitude + '&current=weather_code');
    xhr.send();
  }, function() {
    sendWeather(WEATHER_FAILED);
  }, { timeout: 15000, maximumAge: 600000 });
}

// The phone app percent-decodes the response once; the emulator doesn't.
function parseResponse(response) {
  if (!response) {
    return null;
  }
  try {
    return JSON.parse(response);
  } catch (e) {}
  try {
    return JSON.parse(decodeURIComponent(response));
  } catch (e) {
    return null;
  }
}

Pebble.addEventListener('ready', function() {
  Pebble.sendAppMessage({ JSReady: 1 });
});

Pebble.addEventListener('appmessage', function(e) {
  if (e.payload.WeatherRequest) {
    fetchWeather();
  }
});

Pebble.addEventListener('showConfiguration', function() {
  var saved = localStorage.getItem('settings') || '';
  Pebble.openURL(CONFIG_URL + (saved ? '#' + encodeURIComponent(saved) : ''));
});

Pebble.addEventListener('webviewclosed', function(e) {
  var settings = parseResponse(e.response);
  if (settings) {
    localStorage.setItem('settings', JSON.stringify(settings));
    Pebble.sendAppMessage(settings);
  }
});
