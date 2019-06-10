//http://w3c.github.io/html-reference/input.color.html

var client,textBox;

var mqtt_host = "10.0.0.42";
var mqtt_port = 1884;
var durationEdit = document.getElementById("inDuration");
var colorPicker = document.getElementById("inColor");
var rangeSlider = document.getElementById("inRange");
var rangeLabel = document.getElementById("rangeLabel");

// called when the client connects
function onConnect() {
  // Once a connection has been made, make a subscription and send a message.
  console.log("onConnect");
}

// called when the client loses its connection
function onConnectionLost(responseObject) {
  if (responseObject.errorCode !== 0) {
    console.log("onConnectionLost:"+responseObject.errorMessage);
  }
}

// called when a message arrives
function onMessageArrived(message) {
  console.log(message.destinationName	+ " : "+message.payloadString);
}

function sendMotorPosition(pos){
  client.send("esp/motor/pwm",pos.toString()); 
}

function setup_buttons(){
  var btnOff = document.getElementById("btnOff");
  var btnLeft = document.getElementById("btnLeft");
  var btnMiddle = document.getElementById("btnMiddle");
  var btnRight = document.getElementById("btnRight");
  var btnSend = document.getElementById("btnCustom");

  btnOff.onclick          = function() { client.send("esp/motor/pwm",'0');  }
  btnLeft.onclick         = function() { client.send("esp/motor/pwm",'0');  }
  btnMiddle.onclick    = function() { client.send("esp/motor/pwm",'45');  }
  btnRight.onclick      = function() { client.send("esp/motor/pwm",'90');  }
  btnSend.onclick       = function() { client.send("esp/motor/pwm",rangeSlider.value.toString());  }

}

function init(){
  // Create a client instance
  client = new Paho.MQTT.Client(mqtt_host, Number(mqtt_port), "leds_webapp");
  // set callback handlers
  client.onConnectionLost = onConnectionLost;
  client.onMessageArrived = onMessageArrived;

  // connect the client
  client.connect({onSuccess:onConnect});

  rangeSlider.oninput = function() {
    rangeLabel.innerHTML = "position "+this.value+" °";
  }

  rangeSlider.onclick = function() { client.send("esp/motor/pwm",rangeSlider.value.toString());  }

  
}

//----------------------------------------------------------------------------------


//main();

//setup_buttons();

export{init,setup_buttons}
