// Copyright (c) 2013, Cloudera, inc.

var gauges = [];
var last_value = [];
var max_value = [];
var span_ids = [];
var last_update = Date.now();
var num_gauges = 0;

// TODO allow setting this from the UI, e.g., add a pick-and-choose
// dialog box, or let a user click an 'x' next to a metric to delete
// it from the view.
var url_params = urlParams();
var metrics_uri = makeMetricsUri();

var MAX_SCALE_FACTOR = 1;

// Technically we can just pass the string through as-is without
// having to put it all into a map and re-encode it. However, this
// makes it easier to add new UI functionality (e.g., pick and
// choose).
function urlParams() {
  var query_params = {};
  var query = window.location.search.substring(1);
  var query_vars = query.split('&');
  for (var i = 0; i < query_vars.length; i++) {
    var entry_pair = query_vars[i].split('=');
    if (entry_pair.length == 1) {
      query_params[decodeURIComponent(entry_pair[0])] = true;
    } else {
      query_params[decodeURIComponent(entry_pair[0])] =
        decodeURIComponent(entry_pair[1]);
    }
  }
  return query_params;
}

function makeMetricsUri() {
  var BASE_METRICS_URI = "/jsonmetricz";

  var components = [];
  for (var key in url_params) {
    var value = url_params[key];
    var component = encodeURIComponent(key);
    if (typeof value === 'string') {
      component += '=' + encodeURIComponent(value);
    }
    components.push(component);
  }
  var ret = BASE_METRICS_URI;
  if (components.length > 0) {
    ret += '?' + components.join('&');
  }
  return ret;
}

function generateConfig(name, label, min, max) {
  var config = {
    size: 250,
    label: "",
    min: (min === undefined) ? 0 : min,
    max: (max === undefined || max === 0) ? 1 : max,
    minorTicks: 5
  }

  var range = config.max - config.min;
  config.yellowZones = [{ from: config.min + range*0.75, to: config.min + range*0.9 }];
  config.redZones = [{ from: config.min + range*0.9, to: config.max }];

  return config;
}

function createGauge(name, label, min, max) {
  var config = generateConfig(name, label, min, max);

  var span_id = 'gauge_' + num_gauges++;
  span_ids[name] = span_id;

  var div = d3.select('#gauges').append('div');
  div.attr('class', 'gauge-container');

  var name_elem = div.append('div');
  name_elem.attr('class', 'metric-name');
  name_elem.text(name);

  var display_elem = div.append('div');
  display_elem.attr('id', span_id);
  display_elem.attr('class', 'metric-gauge-display');

  var label_elem = div.append('div');
  label_elem.attr('class', 'metric-label');
  label_elem.html(label);

  gauges[name] = new Gauge(span_id, config);
  gauges[name].render();
}

// Allow for resetting the max over time.
function reconfigureGauge(name, label, min, max) {
  var config = generateConfig(name, label, min, max);
  if (name in gauges) {
    gauges[name].configure(config);
    // Clear the existing rendering, we have to render a whole new gauge.
    d3.select('#' + span_ids[name]).html("");
    gauges[name].render();
  }
}

function updateGauges(json, error) {
  if (error) return console.warn(error);
  if (! json) {
    // Unclear why, but sometimes we seem to get a null
    // here.
    console.warn("null JSON response");
    setTimeout(function() {
      d3.json(metrics_uri, updateGauges);
    }, 1000);
    return;
  }

  var now = Date.now();
  var delta_time = now - last_update;

  json['metrics'].forEach(function(m) {
    var name = m['name'];
    var type = m['type'];
    var unit = m['unit'];
    var label = m['description'];

    var display_value = 0;

    var first_run = false;
    if (!(name in gauges)) {
      first_run = true;
      last_value[name] = 0;
      max_value[name] = 0;
    }

    // For counters, we display a rate
    if (type == "counter") {
      label += "<br>(shown: rate in " + unit + " / second)";
      var cur_value = m['value'];
      if (first_run) {
        last_value[name] = cur_value;
        // display value is 0 to start
      } else {
        var delta_value = cur_value - last_value[name];
        last_value[name] = cur_value;
        var rate = delta_value / delta_time * 1000;
        display_value = rate;
      }

    // For gauges, we simply display the value.
    } else if (type == "gauge") {
      display_value = m['value'];

    // For histograms, we display the 99th percentile (and max).
    } else if (type == "histogram") {
      label += "<br>(shown: 99th percentile)";
      display_value = m['percentile_99'];

    // For non-special-cased stuff just print the value field as well, if available.
    } else {
      if ("value" in m) {
        display_value = m['value'];
      }
    }

    if (first_run) {
      createGauge(name, label, 0, 0);
    }

    // Did max increase? If so, reconfigure.
    if (display_value > max_value[name]) {
      max_value[name] = display_value;
      reconfigureGauge(name, label, 0, Math.floor(max_value[name] * MAX_SCALE_FACTOR));
    }

    gauges[name].redraw(display_value);
  });

  last_update = now;

  setTimeout(function() {
    d3.json(metrics_uri, updateGauges);
  }, 300);
}

function initialize() {
  d3.json(metrics_uri, updateGauges);
}
