<?hh

function non_policied($fn) {
  if ($fn) $fn(null);
}

function policied_local($fn)[policied_local] {
  if ($fn) $fn(null);
}

function policied_shallow($fn)[policied_shallow] {
  if ($fn) $fn(null);
}

function policied($fn)[policied] {
  if ($fn) $fn(null);
}

function policied_of_local($fn)[policied_of_local] {
  if ($fn) $fn(null);
}

function policied_of_shallow($fn)[policied_of_shallow] {
  if ($fn) $fn(null);
}

function policied_of($fn)[policied_of] {
  if ($fn) $fn(null);
}

function pure($fn)[] {
  if ($fn) $fn(null);
}

<<__EntryPoint>>
function main()[policied_of_local] {
  $functions = vec[
    'non_policied',
    'policied_local',
    'policied_shallow',
    'policied',
    'policied_of_local',
    'policied_of_shallow',
    'policied_of',
    'pure'
  ];
  foreach ($functions as $caller) {
    foreach ($functions as $callee) {
      echo "$caller -> $callee:";
      $caller($callee);
      echo " ok\n";
    }
  }
}
